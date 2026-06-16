// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Video Decoder driver
 *
 * Copyright (C) 2019 Collabora, Ltd.
 *
 * Based on rkvdec driver by Google LLC. (Tomasz Figa <tfiga@chromium.org>)
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#include <linux/hw_bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>

#include <linux/dmaengine.h>

#include "rkvdec.h"
#include "rkvdec-regs.h"
#include "rkvdec-vdpu381-regs.h"
#include "rkvdec-vdpu383-regs.h"
#include "rkvdec-rcb.h"
#include "rkvdec-link.h"

/*
 * Bug-A dmaengine experiment (see ISSUE_1_DMAENGINE_DESIGN_2026-05-24.md).
 *
 * Phase 0 (default): no change, no risk. Module behaves exactly as before.
 * Phase 1: in probe(), request a DMA_MEMCPY-capable channel; log result;
 *          release on remove. Tests whether the PL330 dmaengine API
 *          works at all from this driver. Pure side-effect, no risk to
 *          the decode path.
 * Phase 2: phase 1 + on first run(), do a memcpy on a dma_alloc_coherent
 *          test buffer (no V4L2 buffer involvement). Tests whether the
 *          PL330 channel can actually transfer to/from physical memory
 *          addressable via dma_addr_t.
 * Phase 3: phase 2 + try memcpy between two real V4L2 CAPTURE buffer
 *          DMA addresses (resolved via iommu_iova_to_phys). HIGH RISK:
 *          this is the path that needs to work for the real Bug-A fix,
 *          and where the previous CPU-mapped memcpy attempts wedged the
 *          kernel.
 *
 * Walk: 1 → 2 → 3, each only after the previous proves safe.
 */
int vp9_bug_a_phase;
module_param(vp9_bug_a_phase, int, 0644);
MODULE_PARM_DESC(vp9_bug_a_phase,
		 "Bug-A dmaengine workaround phase (0=off, 1=acquire chan only, 2=coherent-buf memcpy, 3=V4L2-buf memcpy)");

int rkvdec_link_mode;
module_param(rkvdec_link_mode, int, 0644);
MODULE_PARM_DESC(rkvdec_link_mode,
		 "VDPU383 LINK mode (0=single-shot default, 1=link-mode). Scopes to VP9 only in Phase 2.");

/*
 * Link-mode pipeline depth (LINK_MODE_PORT_DESIGN_2026-06-01). job_ready
 * backpressures m2m at this many tasks in flight. device_run job_finishes
 * at submit time so m2m keeps dispatching until the ring reaches this
 * depth; the completion path (watchdog poll) reaps tasks and try_schedules
 * more. Default 1 = correctness-parity gate (one task in flight, identical
 * timing to single-shot); raise to 2/4/8 once parity is proven to engage
 * the continuous-armed append path the alt-ref cure needs.
 */
int rkvdec_link_depth = 1;
module_param(rkvdec_link_depth, int, 0644);
MODULE_PARM_DESC(rkvdec_link_depth,
		 "Link-mode tasks in flight before job_ready backpressures (default 1).");

/*
 * rkvdec_submit_mode — VDPU383 submit runtime (vdpu383-submit PLAN, Phase 1).
 *
 *   0 = single-shot / legacy (default). Completion handling is exactly as
 *       before: threaded-IRQ inline reap (rkvdec_irq_handler) for link mode,
 *       or the per-variant single-shot handler.
 *   2 = kthread-worker link completion. Requires rkvdec_link_mode=1. The
 *       hardirq top-half acks the link IRQ and wakes a resident kthread
 *       (rkvdec->link_worker); the worker does the (sleeping) per-slot
 *       writeback reap out of hardirq/threaded-IRQ context. This is the
 *       Phase-1a foundation for the MPP-style kthread runtime that removes
 *       the per-frame threaded-IRQ ms-scale wake on the completion path
 *       (the throughput ceiling root-caused in SESSION_2026-06-07_h264_linkmode).
 *
 * Value 1 is reserved (legacy link == today's rkvdec_link_mode=1 path); the
 * kthread runtime is selected explicitly with 2 so single-shot and the proven
 * link path stay byte-for-byte unchanged at the default.
 */
int rkvdec_submit_mode;
module_param(rkvdec_submit_mode, int, 0644);
MODULE_PARM_DESC(rkvdec_submit_mode,
		 "VDPU383 submit runtime: 0=single-shot/legacy (default), 2=kthread-worker link completion (needs rkvdec_link_mode=1).");

/*
 * rkvdec_link_poll — vdpu383-submit PLAN Phase 1b (the throughput lever).
 *
 * Only meaningful with rkvdec_submit_mode=2. Default 0 = Phase 1a behaviour
 * (worker does ONE reap per IRQ wake, then sleeps).
 *
 * =1: the worker stays HOT — after reaping completed slots (the reap inline-
 * feeds the next frame via v4l2_m2m_try_schedule -> device_run), it POLLS the
 * per-slot writeback for the next completion instead of returning and waiting
 * for the next IRQ wake. This removes the kthread/threaded wake-from-idle
 * latency between frames (the "done->next-run gap" that bounds depth>1 below
 * single-shot — SESSION_2026-06-07_kthread_phase1a measured Phase 1a reap-only
 * at 88.9 fps == legacy, proving the gap is the per-frame wake, not the reap).
 * Bounded by a deadline + cpu_relax/cond_resched so a stuck slot falls through
 * to the fallback watchdog rather than pinning a CPU.
 */
int rkvdec_link_poll;
module_param(rkvdec_link_poll, int, 0644);
MODULE_PARM_DESC(rkvdec_link_poll,
		 "Phase 1b: keep the link worker hot — poll for completion + re-feed instead of sleeping between frames (needs rkvdec_submit_mode=2). Default 0.");

/*
 * R35 (2026-06-01) — knob to toggle warmup-on-resume A/B without rebuilding.
 * Default 0 (R23 baseline behaviour: warmup at probe only). Set to 1 to
 * mirror the BSP's rkvdec2_runtime_resume re-warmup.
 */
int r35_warmup_on_resume = 1;	/* default ON: BSP-faithful rk3576 warmup at
				 * runtime resume eliminates the VDPU383 H.264
				 * deblock race (validated 2026-06-06). Set 0 to
				 * A/B against the un-primed baseline. */
module_param(r35_warmup_on_resume, int, 0644);
MODULE_PARM_DESC(r35_warmup_on_resume,
		 "R35: re-run RK3576 HW warmup on every pm_runtime_resume (0=off/R23, 1=on/BSP-parity)");

/* A/B: disable the RK3576 probe warmup entirely. Added on-board for the
 * 2026-06-16 AV1 per-module-load pinning investigation (does the H.264-shaped
 * warmup poison the AV1 metastable state?). Result: NO — warmup_off=1 gives the
 * same wrong-attractor distribution as warmup-on, so the warmup is not the pin. */
int warmup_off;
module_param(warmup_off, int, 0644);
MODULE_PARM_DESC(warmup_off, "A/B: disable RK3576 warmup entirely (probe+resume) (0=on/default,1=off)");

/*
 * 2026-06-08 Variant B — IRQ-driven warmup (Nicolas's lore suggestion). When 1,
 * the warmup arms the link IRQ and sleeps on a completion (reaped by
 * rkvdec_irq_top) instead of busy-polling reg 0x4c. Default 0 = the validated
 * polled path. Correctness must be identical (warmup primes the same HW state);
 * this only changes how completion is detected.
 */
int warmup_irq;
module_param(warmup_irq, int, 0644);
MODULE_PARM_DESC(warmup_irq,
		 "Variant B: reap the RK3576 warmup by IRQ instead of busy-poll (0=off/poll, 1=on/IRQ)");

/*
 * R36 (2026-06-01) — warmup on every start_streaming(OUTPUT).
 *
 * R35 hooked runtime_resume but `runtime_status=active` persists across
 * gst sessions (something holds a ref), so resume only fires at probe.
 * Per-session hooks are start_streaming(OUTPUT) — exactly one per gst
 * open of /dev/video-dec0. This is what we want for "re-establish
 * known-good HW state at session start" if MPP CLI sustains that state
 * by sheer continuous decode.
 *
 * Default 0; param-gated for A/B against R35-off baseline.
 */
int r36_warmup_on_start_streaming;
module_param(r36_warmup_on_start_streaming, int, 0644);
MODULE_PARM_DESC(r36_warmup_on_start_streaming,
		 "R36: re-run RK3576 HW warmup at every start_streaming(OUTPUT) (0=off, 1=on)");

/*
 * R38-A (2026-06-01) — write 0x30000 (CORE_WORK_MODE BIT(16) |
 * CCU_WORK_MODE BIT(17)) to link_base + 0x48 before each task kick.
 *
 * BSP rkvdec2_link_run (mpp_rkvdec2_link.c:1987-1988) does this per
 * task as the FIRST thing in the run path. We don't.
 *
 * CCU_WORK_MODE is multicore-specific and shouldn't matter on RK3576
 * (single core), but CORE_WORK_MODE BIT(16) may switch IP behaviour.
 *
 * R38 source-diff identified this as the cleanest untested gap
 * after R34/R35/R36 falsified the warmup chain. Default 0 = off.
 */
int r38_a_core_work_mode;
module_param(r38_a_core_work_mode, int, 0644);
MODULE_PARM_DESC(r38_a_core_work_mode,
		 "R38-A: write 0x30000 (CORE_WORK_MODE | CCU_WORK_MODE) to link_base+0x48 before each task kick (0=off, 1=on)");

/*
 * R41 (2026-06-01) — re-run the RK3576 HW warmup after soft-reset on
 * silent_completion. R40 narrowed the bistability to alt-ref decode
 * intermittently hanging HW. Soft-reset puts HW back in a workable
 * state for the in-handler ack-only path, but subsequent decodes
 * cascade-fail — suggesting the soft-reset doesn't fully re-establish
 * the IP-internal state needed for VP9 decode. Re-running the
 * warmup descriptor after soft-reset may restore that state, the way
 * a fresh insmod does for the first ~7 frames.
 */
int r41_warmup_after_reset;
module_param(r41_warmup_after_reset, int, 0644);
MODULE_PARM_DESC(r41_warmup_after_reset,
		 "R41: re-run RK3576 warmup after soft-reset on silent_completion (0=off, 1=on)");

/*
 * R42 (2026-06-01) — IRQ-handler state-settle delay (us). The single-shot
 * IRQ handler needs work between IRQ ack and return for HW state to
 * settle before the next vp9_run's kick. Empirically the per-IRQ
 * pr_info that was there provided this. Default 50.
 */
int r42_irq_settle_us = 0;
module_param(r42_irq_settle_us, int, 0644);
MODULE_PARM_DESC(r42_irq_settle_us,
		 "R42: usleep_range (us) in vp9_run pre-kick for HW state-settle (default 0 - no clear win)");

/*
 * 2026-06-05 throughput experiment — hardirq top-half + WQ_HIGHPRI
 * bottom-half IRQ split for the single-shot vdpu383 path. The current
 * pure-threaded IRQ (NULL primary, IRQF_ONESHOT) wakes a kthread from idle
 * with ms-scale latency, which dominates the done->next-run gap (~9.6 ms,
 * vp9_2026_05_24_session_e). With the split on, a real primary hardirq
 * classifies + acks the IRQ inline (hardirq-safe MMIO only) and defers the
 * sleeping completion (iommu_restore + done()->v4l2_ctrl_find + job_finish)
 * to rkvdec->irq_done_work on system_highpri_wq.
 *
 * Default 0 = legacy pure-threaded IRQ, byte-for-byte unchanged behaviour
 * (top-half returns IRQ_WAKE_THREAD immediately). 1 = split on. Link mode
 * and non-vdpu383 variants always use the thread regardless (their handlers
 * call sleeping functions inline and are not split-safe).
 *
 * 2026-06-05 RESULT (SESSION_2026-06-05_FINALISATION.md): NEGATIVE. The split
 * is correctness-safe (allkey byte-identical to split=0) and hang-free (the
 * dual-defer avoided the session-F sleep-in-atomic), but (a) gives NO reliable
 * throughput win — BBB fps swings 3.3-177 fps run-to-run from CPU-governor /
 * idle-wakeup state, which dominates any split effect — and (b) has a real
 * DESIGN FLAW when enabled: a single irq_done_work + scratch slot means
 * queue_work() on an already-pending work is a no-op, so a completion arriving
 * before the prior work runs is DROPPED, recovered only by the watchdog. Under
 * sustained/cascade load this produced an 867-872 timeouts/run storm (vs ~14
 * at split=0). A viable design needs a per-task completion queue (lockless ring
 * drained by one re-armed worker), not a single work item. Left default-off as
 * a documented probe + scaffolding for that future attempt. DO NOT enable in
 * production.
 */
int irq_split;
module_param(irq_split, int, 0644);
MODULE_PARM_DESC(irq_split,
		 "EXPERIMENT (negative, default 0): hardirq top-half + WQ bottom-half for single-shot vdpu383. =1 drops overlapping completions -> watchdog timeout storm under load. Do not enable.");

/*
 * 2026-06-05 throughput diagnostic. Measures pure HW decode time (single-shot
 * kick -> DONE IRQ) and prints the mean every 100 decoded frames, so decode
 * rate can be separated from gst-pipeline / frame-copy overhead (which scales
 * with frame size and inflates the via-gst fps at 4K). Inert when 0.
 */
int vp9_time;
module_param(vp9_time, int, 0644);
MODULE_PARM_DESC(vp9_time,
		 "diagnostic: print mean pure-HW decode time (kick->DONE IRQ) every 100 frames, ALL codecs single-shot (h264/hevc/vp9/av1) — gst-free driver throughput (0=off)");

/*
 * 2026-06-11 FBC reference-compression throughput probe (increment 1, H.264
 * 8-bit NV12 only). When set, CAPTURE buffers are laid out in AFBC format and
 * the decoder writes/reads references compressed (reg009.fbc_e + payload bases),
 * ~halving reference-read bandwidth (see docs/rk3576/FBC_THROUGHPUT_PLAN.md).
 * Default 0 = linear (production path, byte-identical to before). Measure the
 * win via the vp9_time AXI rd_total_bytes counter, FBC on vs off, same stream.
 * UNVALIDATED on-board: the FBC buffer math can IOMMU-fault if the strides are
 * wrong — only enable on the dev board with the D-state abort guard.
 */
int fbc_enable;
module_param(fbc_enable, int, 0644);
MODULE_PARM_DESC(fbc_enable,
		 "FBC reference-compression probe, H.264 8-bit only: 0=linear (default), 1=AFBC capture buffers + compressed ref reads (UNVALIDATED, dev-board only)");

/*
 * 2026-06-12 FBC diagnostic: log the FBC address layout (decout/payload offset/
 * buffer size/colmv + per-ref payload bases) for the first N decodes so a fault
 * iova from rk_iommu can be correlated against the computed addresses to find the
 * cold-cache reference-read fault. 0 = off. Set e.g. fbc_log=3 before a decode.
 */
int fbc_log;
module_param(fbc_log, int, 0644);
MODULE_PARM_DESC(fbc_log, "FBC diagnostic: log address layout for first N decodes (0=off)");

/*
 * 2026-06-05 throughput fix override. The single-shot kick used to flush the
 * whole IOMMU TLB before EVERY decode (~1.3-2.5 ms/frame); measured
 * byte-identical output without it because vb2 IOVAs are stable frame-to-frame,
 * so the per-frame flush is redundant in steady state. Default behaviour is now
 * flush-only-after-restore (rkvdec->iommu_needs_flush, set on iommu_restore /
 * queue (re)start / init). This param overrides for debug/A-B:
 *   0 = flush-only-after-restore (default, the fix)
 *   1 = never flush (debug; unsafe across error recovery)
 *   2 = always flush every kick (old behaviour; regression/A-B baseline)
 */
int vp9_skip_tlb_flush;
module_param(vp9_skip_tlb_flush, int, 0644);
MODULE_PARM_DESC(vp9_skip_tlb_flush,
		 "IOMMU TLB flush policy: 0=after-restore-only (default fix), 1=never (debug), 2=always (old behaviour)");

/*
 * R43 (2026-06-01) — invalidate prob_ctx_valid on decode failure.
 * Hypothesis: when HW hangs mid-decode (alt-ref especially), it may
 * have partially written to prob_loop[ctx_id] before stalling.
 * Subsequent frames using that context would read garbage adapted
 * probabilities, propagating the failure as a cascade.
 *
 * Mirrors part of MPP HAL behaviour at hal_vp9d_vdpu383.c:373-381
 * where MPP invalidates prob_ctx_valid on intra/error_resilient
 * frames to prevent stale prob_loop propagation.
 */
int r43_invalidate_probs_on_fail;
module_param(r43_invalidate_probs_on_fail, int, 0644);
MODULE_PARM_DESC(r43_invalidate_probs_on_fail,
		 "R43: invalidate prob_ctx_valid[ctx_idx] on decode failure to prevent garbage-prob cascade (0=off, 1=on)");

/*
 * R44 (2026-06-01) — dump register state at vp9_run kick for alt-ref
 * (hidden) frames. Captures all four register substruct regions to
 * dmesg via print_hex_dump. Lets us A/B-diff a successful alt-ref
 * decode vs a hung alt-ref decode on the same content (bistability
 * gives us both within a few trials).
 */
int r44_dump_altref_regs;
module_param(r44_dump_altref_regs, int, 0644);
MODULE_PARM_DESC(r44_dump_altref_regs,
		 "R44: dump common/h26x/common_addr/vp9_addr to dmesg at vp9_run kick on alt-ref frames only (0=off, 1=on)");

/*
 * R45 (2026-06-01) — soft-reset HW IP before alt-ref decode.
 *
 * R44 dumps proved registers we program for alt-ref are byte-identical
 * between perfect and hung trials. The divergence is in HW state we
 * can't observe — residual IP-internal state (caches, pipeline fifos,
 * AXI buffers) from prior decodes. Hypothesis: soft-reset HW IP
 * before each alt-ref decode to clear that residual state.
 */
/* 2026-06-10 — full IOMMU HW refresh before each decode, mirroring the BSP service's
 * mpp_iommu_refresh (rockchip_iommu_disable+enable). Mainline equivalent: the empty-domain
 * dance (rkvdec_iommu_restore = attach empty domain -> reprogram DTE -> detach -> restore
 * default) + an explicit iotlb flush. Tests the stale-IOMMU-HW-state hypothesis for the AV1/VP9
 * non-determinism (stronger than the iotlb-flush alone, which was negative). 0=off. */
int iommu_refresh_per_decode;
module_param(iommu_refresh_per_decode, int, 0644);
MODULE_PARM_DESC(iommu_refresh_per_decode,
		 "Full IOMMU HW refresh (empty-domain DTE reprogram + flush) before each decode; 0=off");

int r45_reset_before_altref;
module_param(r45_reset_before_altref, int, 0644);
MODULE_PARM_DESC(r45_reset_before_altref,
		 "R45: soft-reset HW IP block before each alt-ref decode (0=off, 1=on)");

/*
 * R46 (2026-06-01) — explicit dma_sync_single_for_device on link-mode
 * descriptor slots after rkvdec_link_fill_descriptor writes.
 *
 * LINK_MODE_BASELINE_2026-05-30 doc identified cache coherency between
 * descriptor write and HW read as the most likely cause of link mode's
 * 142/243 → 36/243 Fluster regression (HW receives byte-identical regs
 * via descriptor but produces wrong output). dma_alloc_coherent should
 * make sync unnecessary, but rk3576's IOMMU may not fully participate
 * in the coherency domain.
 */
int r46_link_desc_sync;
module_param(r46_link_desc_sync, int, 0644);
MODULE_PARM_DESC(r46_link_desc_sync,
		 "R46: dma_sync_single_for_device after link descriptor fill (0=off, 1=on)");

/*
 * R49 (2026-06-02) — MPP HAL pre_mv carry-forward for reg217.
 *
 * MPP HAL maintains `pre_mv_base_addr` as a persistent value that
 * only updates when current frame is visible INTER (not
 * intra_only, not error_resilient, last_show_frame). For alt-ref
 * decode, this means reg217 points at the LAST SHOWN INTER's colmv
 * — not the previous frame's colmv if that previous frame was
 * itself an alt-ref.
 *
 * Our existing code (without R49) tracks only `last` and self-refs
 * when last was alt-ref. R49 = MPP-faithful behaviour for the alt-
 * ref-after-alt-ref case specifically. Tests whether this is the
 * alt-ref intermittent hang trigger.
 *
 * Previous test (vp9_bug_a_phase=8, session-h 2026-05-25) tested
 * an MPP-style override of reg217 but targeted the all-zero-Y
 * symptom. This re-tests against the HANG symptom with proper
 * persistence tracking.
 */
int r49_pre_mv_carryforward;
module_param(r49_pre_mv_carryforward, int, 0644);
MODULE_PARM_DESC(r49_pre_mv_carryforward,
		 "R49: MPP-style pre_mv carry-forward across alt-refs for reg217 (0=off, 1=on)");

/*
 * R50 (2026-06-02) — production-pragmatic alt-ref skip.
 *
 * When set, vp9_run detects alt-ref frames (SHOW_FRAME clear,
 * not KEY, not INTRA_ONLY) and skips the HW decode entirely:
 * mark the m2m buffer DONE without kicking HW, advance DPB
 * tracking, return immediately.
 *
 * Trade-off: alt-ref CAPTURE buffer contents undefined; subsequent
 * INTER frames that reference the alt-ref slot will read garbage
 * for MV/pixel data, producing visual artifacts (this is the
 * Bug-A class of corruption). But the alt-ref intermittent hang
 * (R44/R45) is completely prevented — every session decodes the
 * non-alt-ref frames cleanly.
 *
 * R34-R49 value/sequence hunting all hit walls. R44 proved
 * registers byte-identical between OUR perfect and hung trials,
 * so bistability is at a HW-state layer below what registers
 * can express. Skip-the-decode is the production-pragmatic
 * backstop — accepts visual artifact instead of hang.
 */
int r50_skip_altref;
module_param(r50_skip_altref, int, 0644);
MODULE_PARM_DESC(r50_skip_altref,
		 "R50: skip alt-ref decode (no HW kick, return DONE immediately) for production hang-safety (0=off, 1=on)");

/*
 * R47 (2026-06-01) — stronger barrier before CFG_DONE in link
 * enqueue. LINK_MODE_BASELINE_2026-05-30 hypothesis #4: link
 * mode uses only wmb() before CFG_DONE; single-shot uses
 * wmb()+readl(reg15) to drain posted writes. Adds readl drains
 * + mb() before CFG_DONE.
 */
int r47_link_barrier_strong;
module_param(r47_link_barrier_strong, int, 0644);
MODULE_PARM_DESC(r47_link_barrier_strong,
		 "R47: stronger barrier before link CFG_DONE (readl drain + mb) (0=off, 1=on)");

static struct dma_chan *vp9_bug_a_chan;
static bool vp9_bug_a_phase3_attempted;

/*
 * Synchronous PL330 memcpy from src_iova to dst_iova, both in rkvdec's
 * IOMMU domain. Resolves to physical via iommu_iova_to_phys (PL330 has
 * no IOMMU on RK3576 — it accesses physical memory directly, verified
 * via /sys/devices/.../2ab90000.dma-controller has no iommu_group).
 *
 * Returns 0 on success, negative errno on failure.
 *
 * Caller context: m2m worker (sleep-allowed) — uses mdelay polling for
 * completion since PL330 memcpys complete in <1 ms on small transfers
 * and we want completion guaranteed before the HW kick.
 *
 * NOTE: assumes the IOVAs are in rkvdec's own iommu_domain (the only
 * domain that can resolve V4L2 dma-contig buffer addresses for this
 * driver).
 */
int rkvdec_bug_a_copy(struct rkvdec_dev *rkvdec,
		      dma_addr_t dst_iova, dma_addr_t src_iova, size_t len)
{
	struct iommu_domain *dom;
	phys_addr_t src_phys, dst_phys;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	enum dma_status status;
	int wait_ms = 100;

	if (!vp9_bug_a_chan)
		return -ENODEV;

	dom = iommu_get_domain_for_dev(rkvdec->dev);
	if (!dom)
		return -ENODEV;

	src_phys = iommu_iova_to_phys(dom, src_iova);
	dst_phys = iommu_iova_to_phys(dom, dst_iova);
	if (!src_phys || !dst_phys)
		return -EFAULT;

	tx = dmaengine_prep_dma_memcpy(vp9_bug_a_chan,
				       (dma_addr_t)dst_phys,
				       (dma_addr_t)src_phys,
				       len, DMA_PREP_INTERRUPT);
	if (!tx)
		return -ENOMEM;

	cookie = dmaengine_submit(tx);
	dma_async_issue_pending(vp9_bug_a_chan);

	while (wait_ms-- > 0) {
		status = dma_async_is_tx_complete(vp9_bug_a_chan, cookie,
						  NULL, NULL);
		if (status == DMA_COMPLETE)
			return 0;
		mdelay(1);
	}
	return -ETIMEDOUT;
}

static bool rkvdec_image_fmt_match(enum rkvdec_image_fmt fmt1,
				   enum rkvdec_image_fmt fmt2)
{
	return fmt1 == fmt2 || fmt2 == RKVDEC_IMG_FMT_ANY ||
	       fmt1 == RKVDEC_IMG_FMT_ANY;
}

static bool rkvdec_image_fmt_changed(struct rkvdec_ctx *ctx,
				     enum rkvdec_image_fmt image_fmt)
{
	if (image_fmt == RKVDEC_IMG_FMT_ANY)
		return false;

	return ctx->image_fmt != image_fmt;
}

static u32 rkvdec_enum_decoded_fmt(struct rkvdec_ctx *ctx, int index,
				   enum rkvdec_image_fmt image_fmt)
{
	const struct rkvdec_coded_fmt_desc *desc = ctx->coded_fmt_desc;
	int fmt_idx = -1;
	unsigned int i;

	if (WARN_ON(!desc))
		return 0;

	for (i = 0; i < desc->num_decoded_fmts; i++) {
		if (!rkvdec_image_fmt_match(desc->decoded_fmts[i].image_fmt,
					    image_fmt))
			continue;
		fmt_idx++;
		if (index == fmt_idx)
			return desc->decoded_fmts[i].fourcc;
	}

	return 0;
}

static bool rkvdec_is_valid_fmt(struct rkvdec_ctx *ctx, u32 fourcc,
				enum rkvdec_image_fmt image_fmt)
{
	const struct rkvdec_coded_fmt_desc *desc = ctx->coded_fmt_desc;
	unsigned int i;

	for (i = 0; i < desc->num_decoded_fmts; i++) {
		if (rkvdec_image_fmt_match(desc->decoded_fmts[i].image_fmt,
					   image_fmt) &&
		    desc->decoded_fmts[i].fourcc == fourcc)
			return true;
	}

	return false;
}

static u32 rkvdec_colmv_size(u16 width, u16 height)
{
	return 128 * DIV_ROUND_UP(width, 16) * DIV_ROUND_UP(height, 16);
}

static u32 vdpu383_colmv_size(u16 width, u16 height)
{
	return ALIGN(width, 64) * ALIGN(height, 16);
}

/*
 * Horizontal stride alignment for VDPU383.
 * Mirrors MPP's mpp_align_128_odd_plus_64():
 *   align to 64; if (val-64) % 256 == 128: return val; else: (ALIGN(val,128)|128)+64
 */
static u32 vdpu383_hor_align(u32 val)
{
	val = ALIGN(val, 64);
	if ((val - 64) % 256 == 128)
		return val;
	return (ALIGN(val, 128) | 128) + 64;
}

static void rkvdec_fill_decoded_pixfmt(struct rkvdec_ctx *ctx,
				       struct v4l2_pix_format_mplane *pix_mp)
{
	const struct rkvdec_variant *variant = ctx->dev->variant;

	v4l2_fill_pixfmt_mp(pix_mp, pix_mp->pixelformat, pix_mp->width, pix_mp->height);

	/*
	 * Apply hardware-required horizontal stride alignment if the variant
	 * needs it (e.g. VDPU383 requires mpp_align_128_odd_plus_64).
	 *
	 * The alignment is in BYTES-PER-LINE, so it must operate on the
	 * bytesperline value v4l2_fill_pixfmt_mp() just computed (which
	 * accounts for the fourcc's bytes-per-pixel ratio: 1 for NV12,
	 * 5/4 for NV15 10-bit packed, 2 for P010 16-bit container, etc.).
	 *
	 * Aligning width directly (the pre-2026-05-24 pattern) silently
	 * under-sized the buffer for non-NV12 formats: e.g. NV15 at width
	 * 1920 has bytesperline 2400, not 1920, so aligning width 1920 to
	 * 1920 leaves the buffer 25 % short and HW overruns it.
	 */
	if (variant->ops->hor_align) {
		u32 aligned_bpl =
			variant->ops->hor_align(pix_mp->plane_fmt[0].bytesperline);

		if (aligned_bpl != pix_mp->plane_fmt[0].bytesperline) {
			/*
			 * VP9 (and similar codecs) write decoded pixels in
			 * superblock-row granularity. For VDPU383 the write
			 * granularity is 8 rows: a non-8-aligned visible height
			 * causes HW to round up and overflow the Y plane into
			 * the UV plane region of the V4L2 buffer.
			 *
			 * Pattern discovered 2026-05-29 via Fluster 03-size
			 * cluster: heights ending mod 8 == 0 (e.g. 200, 208,
			 * 224) pass cleanly; heights ending mod 8 != 0 (e.g.
			 * 196, 198, 202, 210, 226) all fail. Padding the
			 * sizeimage to use ALIGN(height, 8) reserves the
			 * extra Y-plane rows HW will write, keeping the V4L2
			 * UV-plane offset (bytesperline * pix_mp->height)
			 * matched to where HW actually writes UV.
			 *
			 * 1080-tall content: ALIGN(1080, 8) = 1080, no change.
			 * 1088-tall (already aligned 64) content: no change.
			 * Compatible with the prior "don't ALIGN(16) for 1080p"
			 * lesson because 8-row alignment is fine-grained enough
			 * to never bump 1080 past 1080.
			 */
			/*
			 * 2026-05-31 Round 6 attempted ALIGN(h,64) — REVERTED.
			 * Hypothesis was that VP9 SB granularity (64x64) meant
			 * HW writes 64-row blocks regardless of visible height,
			 * so 02-size tiny frames (8x8 etc.) needed full SB
			 * alignment. Wrong: ALIGN(h,64) broke ALL content
			 * (0/64 on 00-quant, 0/66 on 03-size). The extra
			 * padding moved pixel positions off where Fluster's
			 * reference comparator expected them.
			 *
			 * So HW's row-stride and column-stride aren't naive
			 * SB-aligned — it must do some smarter visible-area
			 * write. The 02-size bug remains a correctness
			 * mystery without datasheet.
			 */
			u32 height = ALIGN(pix_mp->height, 8);

			pix_mp->plane_fmt[0].bytesperline = aligned_bpl;
			pix_mp->height = height;
			/* 4:2:0: Y plane is bytesperline * height bytes, UV is
			 * half that (interleaved at half vertical resolution). */
			pix_mp->plane_fmt[0].sizeimage = aligned_bpl * height * 3 / 2;
		}
	}

	/*
	 * FBC probe (fbc_enable=1, increment 1): replace the linear pixel-region
	 * size with the AFBC header+payload layout; colmv then follows it. Only
	 * H.264 config_registers() actually programs fbc_e — other codecs would
	 * just get an over-sized (still linear) buffer here, which is safe (no
	 * overrun), only wasteful. Default-off path is untouched.
	 */
	ctx->fbc_pld_offset = 0;
	ctx->fbc_head_stride = 0;
	if (fbc_enable && pix_mp->pixelformat == V4L2_PIX_FMT_NV12) {
		u32 head_stride, pld_offset, pixel_size;

		vdpu383_fbc_layout(pix_mp->width, pix_mp->height,
				   &head_stride, &pld_offset, &pixel_size);
		ctx->fbc_head_stride = head_stride;
		ctx->fbc_pld_offset = pld_offset;
		pix_mp->plane_fmt[0].sizeimage = pixel_size;
	}

	ctx->colmv_offset = pix_mp->plane_fmt[0].sizeimage;

	pix_mp->plane_fmt[0].sizeimage += variant->ops->colmv_size(pix_mp->width, pix_mp->height);
}

static void rkvdec_reset_fmt(struct rkvdec_ctx *ctx, struct v4l2_format *f,
			     u32 fourcc)
{
	memset(f, 0, sizeof(*f));
	f->fmt.pix_mp.pixelformat = fourcc;
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	f->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	f->fmt.pix_mp.quantization = V4L2_QUANTIZATION_DEFAULT;
	f->fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static void rkvdec_reset_decoded_fmt(struct rkvdec_ctx *ctx)
{
	struct v4l2_format *f = &ctx->decoded_fmt;
	u32 fourcc;

	fourcc = rkvdec_enum_decoded_fmt(ctx, 0, ctx->image_fmt);
	rkvdec_reset_fmt(ctx, f, fourcc);
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f->fmt.pix_mp.width = ctx->coded_fmt.fmt.pix_mp.width;
	f->fmt.pix_mp.height = ctx->coded_fmt.fmt.pix_mp.height;
	rkvdec_fill_decoded_pixfmt(ctx, &f->fmt.pix_mp);
}

static int rkvdec_try_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rkvdec_ctx *ctx = container_of(ctrl->handler, struct rkvdec_ctx, ctrl_hdl);
	const struct rkvdec_coded_fmt_desc *desc = ctx->coded_fmt_desc;

	if (desc->ops->try_ctrl)
		return desc->ops->try_ctrl(ctx, ctrl);

	return 0;
}

static int rkvdec_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rkvdec_ctx *ctx = container_of(ctrl->handler, struct rkvdec_ctx, ctrl_hdl);
	const struct rkvdec_coded_fmt_desc *desc = ctx->coded_fmt_desc;
	enum rkvdec_image_fmt image_fmt;
	struct vb2_queue *vq;

	if (ctrl->id == V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS) {
		ctx->has_sps_st_rps |= !!(ctrl->has_changed);
		return 0;
	}

	if (ctrl->id == V4L2_CID_STATELESS_HEVC_EXT_SPS_LT_RPS) {
		ctx->has_sps_lt_rps |= !!(ctrl->has_changed);
		return 0;
	}

	/*
	 * Mid-stream resolution-change detection. If the just-set control
	 * carries dimensions that differ from the current decoded_fmt,
	 * emit V4L2_EVENT_SOURCE_CHANGE so userspace knows to cycle the
	 * CAPTURE queue (streamoff, free buffers, re-G_FMT, re-REQBUFS,
	 * streamon) at the new size.
	 *
	 * Only codecs whose backend implements check_source_change opt in;
	 * codecs without it retain the legacy behaviour where mid-stream
	 * dimension change is undefined.
	 */
	if (desc->ops->check_source_change &&
	    desc->ops->check_source_change(ctx, ctrl)) {
		static const struct v4l2_event ev_src_ch = {
			.type = V4L2_EVENT_SOURCE_CHANGE,
			.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
		};
		v4l2_event_queue_fh(&ctx->fh, &ev_src_ch);
		/*
		 * Don't update decoded_fmt here — leave the current
		 * dimensions in place until userspace stops CAPTURE and
		 * re-G_FMTs. The next run() will detect the size mismatch
		 * and refuse to decode (rejecting bad output is safer
		 * than producing corrupt frames into wrong-sized buffers).
		 */
	}

	/* Check if this change requires a capture format reset */
	if (!desc->ops->get_image_fmt)
		return 0;

	image_fmt = desc->ops->get_image_fmt(ctx, ctrl);
	if (rkvdec_image_fmt_changed(ctx, image_fmt)) {
		vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
				     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		if (vb2_is_busy(vq))
			return -EBUSY;

		ctx->image_fmt = image_fmt;
		rkvdec_reset_decoded_fmt(ctx);
	}

	return 0;
}

/* Non-static so codec backends in separate compilation units (e.g.
 * rkvdec-vdpu383-vp9.c) can reference this for ctrl_descs that need
 * bit-depth driven image_fmt updates. */
const struct v4l2_ctrl_ops rkvdec_ctrl_ops = {
	.try_ctrl = rkvdec_try_ctrl,
	.s_ctrl = rkvdec_s_ctrl,
};

static const struct rkvdec_ctrl_desc rkvdec_hevc_ctrl_descs[] = {
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS,
		.cfg.flags = V4L2_CTRL_FLAG_DYNAMIC_ARRAY,
		.cfg.type = V4L2_CTRL_TYPE_HEVC_SLICE_PARAMS,
		.cfg.dims = { 600 },
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_SPS,
		.cfg.ops = &rkvdec_ctrl_ops,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_PPS,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_DECODE_MODE,
		.cfg.min = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
		.cfg.max = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
		.cfg.def = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_START_CODE,
		.cfg.min = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
		.cfg.def = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
		.cfg.max = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
		.cfg.min = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
		.cfg.max = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10,
		.cfg.def = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
		.cfg.min = V4L2_MPEG_VIDEO_HEVC_LEVEL_1,
		.cfg.max = V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1,
	},
};

static const struct rkvdec_ctrls rkvdec_hevc_ctrls = {
	.ctrls = rkvdec_hevc_ctrl_descs,
	.num_ctrls = ARRAY_SIZE(rkvdec_hevc_ctrl_descs),
};

static const struct rkvdec_ctrl_desc vdpu38x_hevc_ctrl_descs[] = {
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_SPS,
		.cfg.ops = &rkvdec_ctrl_ops,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_PPS,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_DECODE_MODE,
		.cfg.min = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
		.cfg.max = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
		.cfg.def = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_START_CODE,
		.cfg.min = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
		.cfg.def = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
		.cfg.max = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
		.cfg.min = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
		.cfg.max = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10,
		.cfg.menu_skip_mask =
			BIT(V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE),
		.cfg.def = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
		.cfg.min = V4L2_MPEG_VIDEO_HEVC_LEVEL_1,
		.cfg.max = V4L2_MPEG_VIDEO_HEVC_LEVEL_6_1,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_EXT_SPS_ST_RPS,
		.cfg.ops = &rkvdec_ctrl_ops,
		.cfg.dims = { 65 },
	},
	{
		.cfg.id = V4L2_CID_STATELESS_HEVC_EXT_SPS_LT_RPS,
		.cfg.ops = &rkvdec_ctrl_ops,
		.cfg.dims = { 65 },
	},
};

static const struct rkvdec_ctrls vdpu38x_hevc_ctrls = {
	.ctrls = vdpu38x_hevc_ctrl_descs,
	.num_ctrls = ARRAY_SIZE(vdpu38x_hevc_ctrl_descs),
};

static const struct rkvdec_decoded_fmt_desc rkvdec_hevc_decoded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.image_fmt = RKVDEC_IMG_FMT_420_8BIT,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV15,
		.image_fmt = RKVDEC_IMG_FMT_420_10BIT,
	},
};

static const struct rkvdec_ctrl_desc rkvdec_h264_ctrl_descs[] = {
	{
		.cfg.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_SPS,
		.cfg.ops = &rkvdec_ctrl_ops,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_PPS,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		.cfg.min = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.cfg.max = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.cfg.def = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_START_CODE,
		.cfg.min = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.cfg.def = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.cfg.max = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
		.cfg.min = V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE,
		.cfg.max = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422_INTRA,
		.cfg.menu_skip_mask =
			BIT(V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED) |
			BIT(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE),
		.cfg.def = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL,
		.cfg.min = V4L2_MPEG_VIDEO_H264_LEVEL_1_0,
		.cfg.max = V4L2_MPEG_VIDEO_H264_LEVEL_5_1,
	},
};

static const struct rkvdec_ctrls rkvdec_h264_ctrls = {
	.ctrls = rkvdec_h264_ctrl_descs,
	.num_ctrls = ARRAY_SIZE(rkvdec_h264_ctrl_descs),
};

static const struct rkvdec_ctrl_desc vdpu38x_h264_ctrl_descs[] = {
	{
		.cfg.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_SPS,
		.cfg.ops = &rkvdec_ctrl_ops,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_PPS,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		.cfg.min = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.cfg.max = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.cfg.def = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_START_CODE,
		.cfg.min = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.cfg.def = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.cfg.max = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
		.cfg.min = V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE,
		.cfg.max = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422_INTRA,
		.cfg.menu_skip_mask =
			BIT(V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED) |
			BIT(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE),
		.cfg.def = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL,
		.cfg.min = V4L2_MPEG_VIDEO_H264_LEVEL_1_0,
		.cfg.max = V4L2_MPEG_VIDEO_H264_LEVEL_6_0,
	},
};

static const struct rkvdec_ctrls vdpu38x_h264_ctrls = {
	.ctrls = vdpu38x_h264_ctrl_descs,
	.num_ctrls = ARRAY_SIZE(vdpu38x_h264_ctrl_descs),
};

static const struct rkvdec_decoded_fmt_desc rkvdec_h264_decoded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.image_fmt = RKVDEC_IMG_FMT_420_8BIT,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV15,
		.image_fmt = RKVDEC_IMG_FMT_420_10BIT,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV16,
		.image_fmt = RKVDEC_IMG_FMT_422_8BIT,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV20,
		.image_fmt = RKVDEC_IMG_FMT_422_10BIT,
	},
};

static const struct rkvdec_ctrl_desc rkvdec_vp9_ctrl_descs[] = {
	{
		.cfg.id = V4L2_CID_STATELESS_VP9_FRAME,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_VP9_COMPRESSED_HDR,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_VP9_PROFILE,
		.cfg.min = V4L2_MPEG_VIDEO_VP9_PROFILE_0,
		.cfg.max = V4L2_MPEG_VIDEO_VP9_PROFILE_0,
		.cfg.def = V4L2_MPEG_VIDEO_VP9_PROFILE_0,
	},
};

static const struct rkvdec_ctrls rkvdec_vp9_ctrls = {
	.ctrls = rkvdec_vp9_ctrl_descs,
	.num_ctrls = ARRAY_SIZE(rkvdec_vp9_ctrl_descs),
};

static const struct rkvdec_decoded_fmt_desc rkvdec_vp9_decoded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.image_fmt = RKVDEC_IMG_FMT_420_8BIT,
	},
};

static const struct rkvdec_coded_fmt_desc rkvdec_coded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_HEVC_SLICE,
		.frmsize = {
			.min_width = 64,
			.max_width = 4096,
			.step_width = 64,
			.min_height = 64,
			.max_height = 2304,
			.step_height = 16,
		},
		.ctrls = &rkvdec_hevc_ctrls,
		.ops = &rkvdec_hevc_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec_hevc_decoded_fmts),
		.decoded_fmts = rkvdec_hevc_decoded_fmts,
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.frmsize = {
			.min_width = 64,
			.max_width = 4096,
			.step_width = 64,
			.min_height = 48,
			.max_height = 2560,
			.step_height = 16,
		},
		.ctrls = &rkvdec_h264_ctrls,
		.ops = &rkvdec_h264_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec_h264_decoded_fmts),
		.decoded_fmts = rkvdec_h264_decoded_fmts,
		.subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF,
	},
	{
		.fourcc = V4L2_PIX_FMT_VP9_FRAME,
		.frmsize = {
			.min_width = 64,
			.max_width = 4096,
			.step_width = 64,
			.min_height = 64,
			.max_height = 2304,
			.step_height = 64,
		},
		.ctrls = &rkvdec_vp9_ctrls,
		.ops = &rkvdec_vp9_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec_vp9_decoded_fmts),
		.decoded_fmts = rkvdec_vp9_decoded_fmts,
	}
};

static const struct rkvdec_coded_fmt_desc rk3288_coded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_HEVC_SLICE,
		.frmsize = {
			.min_width = 64,
			.max_width = 4096,
			.step_width = 64,
			.min_height = 64,
			.max_height = 2304,
			.step_height = 16,
		},
		.ctrls = &rkvdec_hevc_ctrls,
		.ops = &rkvdec_hevc_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec_hevc_decoded_fmts),
		.decoded_fmts = rkvdec_hevc_decoded_fmts,
	}
};

static const struct rkvdec_coded_fmt_desc vdpu381_coded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_HEVC_SLICE,
		.frmsize = {
			.min_width = 64,
			.max_width = 65472,
			.step_width = 64,
			.min_height = 64,
			.max_height = 65472,
			.step_height = 16,
		},
		.ctrls = &vdpu38x_hevc_ctrls,
		.ops = &rkvdec_vdpu381_hevc_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec_hevc_decoded_fmts),
		.decoded_fmts = rkvdec_hevc_decoded_fmts,
		.subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF,
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.frmsize = {
			.min_width = 64,
			.max_width =  65520,
			.step_width = 64,
			.min_height = 64,
			.max_height =  65520,
			.step_height = 16,
		},
		.ctrls = &vdpu38x_h264_ctrls,
		.ops = &rkvdec_vdpu381_h264_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec_h264_decoded_fmts),
		.decoded_fmts = rkvdec_h264_decoded_fmts,
		.subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF,
	},
};

extern const struct rkvdec_ctrls rkvdec_vdpu383_vp9_ctrls;
/*
 * Size pinned so ARRAY_SIZE() works at the use site below.
 * [0] = NV12  (Profile 0, 4:2:0 8-bit)
 * [1] = NV15  (Profile 2, 4:2:0 10-bit packed)
 * Selected per-frame via rkvdec_vdpu383_vp9_get_image_fmt() based on
 * the V4L2_CID_STATELESS_VP9_FRAME bit_depth field.
 */
extern const struct rkvdec_decoded_fmt_desc rkvdec_vdpu383_vp9_decoded_fmts[2];
extern const struct rkvdec_ctrls rkvdec_vdpu383_av1_ctrls;
extern const struct rkvdec_decoded_fmt_desc rkvdec_vdpu383_av1_decoded_fmts[1];

static const struct rkvdec_coded_fmt_desc vdpu383_coded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_HEVC_SLICE,
		.frmsize = {
			.min_width = 64,
			.max_width = 65472,
			.step_width = 64,
			.min_height = 64,
			.max_height = 65472,
			.step_height = 16,
		},
		.ctrls = &vdpu38x_hevc_ctrls,
		.ops = &rkvdec_vdpu383_hevc_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec_hevc_decoded_fmts),
		.decoded_fmts = rkvdec_hevc_decoded_fmts,
		.subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF,
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.frmsize = {
			.min_width = 64,
			.max_width =  65520,
			.step_width = 64,
			.min_height = 64,
			.max_height =  65520,
			.step_height = 16,
		},
		.ctrls = &vdpu38x_h264_ctrls,
		.ops = &rkvdec_vdpu383_h264_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec_h264_decoded_fmts),
		.decoded_fmts = rkvdec_h264_decoded_fmts,
		.subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF,
	},
	{
		.fourcc = V4L2_PIX_FMT_VP9_FRAME,
		.frmsize = {
			.min_width = 1,
			.max_width = 65472,
			.step_width = 1,
			.min_height = 1,
			.max_height = 65472,
			.step_height = 1,
		},
		.ctrls = &rkvdec_vdpu383_vp9_ctrls,
		.ops = &rkvdec_vdpu383_vp9_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec_vdpu383_vp9_decoded_fmts),
		.decoded_fmts = rkvdec_vdpu383_vp9_decoded_fmts,
		.subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF,
	},
	{
		.fourcc = V4L2_PIX_FMT_AV1_FRAME,
		.frmsize = {
			.min_width = 64,
			.max_width = 65472,
			.step_width = 64,
			.min_height = 64,
			.max_height = 65472,
			.step_height = 64,
		},
		.ctrls = &rkvdec_vdpu383_av1_ctrls,
		.ops = &rkvdec_vdpu383_av1_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec_vdpu383_av1_decoded_fmts),
		.decoded_fmts = rkvdec_vdpu383_av1_decoded_fmts,
		.subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF,
	},
};

static const struct rkvdec_coded_fmt_desc *
rkvdec_enum_coded_fmt_desc(struct rkvdec_ctx *ctx, int index)
{
	const struct rkvdec_variant *variant = ctx->dev->variant;
	int fmt_idx = -1;
	unsigned int i;

	for (i = 0; i < variant->num_coded_fmts; i++) {
		fmt_idx++;
		if (index == fmt_idx)
			return &variant->coded_fmts[i];
	}

	return NULL;
}

static const struct rkvdec_coded_fmt_desc *
rkvdec_find_coded_fmt_desc(struct rkvdec_ctx *ctx, u32 fourcc)
{
	const struct rkvdec_variant *variant = ctx->dev->variant;
	unsigned int i;

	for (i = 0; i < variant->num_coded_fmts; i++) {
		if (variant->coded_fmts[i].fourcc == fourcc)
			return &variant->coded_fmts[i];
	}

	return NULL;
}

static void rkvdec_reset_coded_fmt(struct rkvdec_ctx *ctx)
{
	struct v4l2_format *f = &ctx->coded_fmt;

	ctx->coded_fmt_desc = rkvdec_enum_coded_fmt_desc(ctx, 0);
	rkvdec_reset_fmt(ctx, f, ctx->coded_fmt_desc->fourcc);

	f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	f->fmt.pix_mp.width = ctx->coded_fmt_desc->frmsize.min_width;
	f->fmt.pix_mp.height = ctx->coded_fmt_desc->frmsize.min_height;

	if (ctx->coded_fmt_desc->ops->adjust_fmt)
		ctx->coded_fmt_desc->ops->adjust_fmt(ctx, f);
}

static int rkvdec_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	struct rkvdec_ctx *ctx = file_to_rkvdec_ctx(file);
	const struct rkvdec_coded_fmt_desc *desc;

	if (fsize->index != 0)
		return -EINVAL;

	desc = rkvdec_find_coded_fmt_desc(ctx, fsize->pixel_format);
	if (!desc)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	fsize->stepwise.min_width = 1;
	fsize->stepwise.max_width = desc->frmsize.max_width;
	fsize->stepwise.step_width = 1;
	fsize->stepwise.min_height = 1;
	fsize->stepwise.max_height = desc->frmsize.max_height;
	fsize->stepwise.step_height = 1;

	return 0;
}

static int rkvdec_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct rkvdec_dev *rkvdec = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, rkvdec->dev->driver->name,
		sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 rkvdec->dev->driver->name);
	return 0;
}

static int rkvdec_try_capture_fmt(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rkvdec_ctx *ctx = file_to_rkvdec_ctx(file);
	const struct rkvdec_coded_fmt_desc *coded_desc;

	/*
	 * The codec context should point to a coded format desc, if the format
	 * on the coded end has not been set yet, it should point to the
	 * default value.
	 */
	coded_desc = ctx->coded_fmt_desc;
	if (WARN_ON(!coded_desc))
		return -EINVAL;

	if (!rkvdec_is_valid_fmt(ctx, pix_mp->pixelformat, ctx->image_fmt))
		pix_mp->pixelformat = rkvdec_enum_decoded_fmt(ctx, 0,
							      ctx->image_fmt);

	/* Always apply the frmsize constraint of the coded end. */
	pix_mp->width = max(pix_mp->width, ctx->coded_fmt.fmt.pix_mp.width);
	pix_mp->height = max(pix_mp->height, ctx->coded_fmt.fmt.pix_mp.height);
	v4l2_apply_frmsize_constraints(&pix_mp->width,
				       &pix_mp->height,
				       &coded_desc->frmsize);

	rkvdec_fill_decoded_pixfmt(ctx, pix_mp);
	pix_mp->field = V4L2_FIELD_NONE;

	return 0;
}

static int rkvdec_try_output_fmt(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rkvdec_ctx *ctx = file_to_rkvdec_ctx(file);
	const struct rkvdec_coded_fmt_desc *desc;

	desc = rkvdec_find_coded_fmt_desc(ctx, pix_mp->pixelformat);
	if (!desc) {
		desc = rkvdec_enum_coded_fmt_desc(ctx, 0);
		pix_mp->pixelformat = desc->fourcc;
	}

	v4l2_apply_frmsize_constraints(&pix_mp->width,
				       &pix_mp->height,
				       &desc->frmsize);

	pix_mp->field = V4L2_FIELD_NONE;
	/* All coded formats are considered single planar for now. */
	pix_mp->num_planes = 1;

	if (desc->ops->adjust_fmt) {
		int ret;

		ret = desc->ops->adjust_fmt(ctx, f);
		if (ret)
			return ret;
	}

	return 0;
}

static int rkvdec_s_capture_fmt(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rkvdec_ctx *ctx = file_to_rkvdec_ctx(file);
	struct vb2_queue *vq;
	int ret;

	/* Change not allowed if queue is busy */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
			     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ret = rkvdec_try_capture_fmt(file, priv, f);
	if (ret)
		return ret;

	ctx->decoded_fmt = *f;
	return 0;
}

static int rkvdec_s_output_fmt(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct rkvdec_ctx *ctx = file_to_rkvdec_ctx(file);
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	const struct rkvdec_coded_fmt_desc *desc;
	struct v4l2_format *cap_fmt;
	struct vb2_queue *peer_vq, *vq;
	int ret;

	/*
	 * In order to support dynamic resolution change, the decoder admits
	 * a resolution change, as long as the pixelformat remains. Can't be
	 * done if streaming.
	 */
	vq = v4l2_m2m_get_vq(m2m_ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (vb2_is_streaming(vq) ||
	    (vb2_is_busy(vq) &&
	     f->fmt.pix_mp.pixelformat != ctx->coded_fmt.fmt.pix_mp.pixelformat))
		return -EBUSY;

	/*
	 * Since format change on the OUTPUT queue will reset the CAPTURE
	 * queue, we can't allow doing so when the CAPTURE queue has buffers
	 * allocated.
	 */
	peer_vq = v4l2_m2m_get_vq(m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(peer_vq))
		return -EBUSY;

	ret = rkvdec_try_output_fmt(file, priv, f);
	if (ret)
		return ret;

	desc = rkvdec_find_coded_fmt_desc(ctx, f->fmt.pix_mp.pixelformat);
	if (!desc)
		return -EINVAL;
	ctx->coded_fmt_desc = desc;
	ctx->coded_fmt = *f;

	/*
	 * Current decoded format might have become invalid with newly
	 * selected codec, so reset it to default just to be safe and
	 * keep internal driver state sane. User is mandated to set
	 * the decoded format again after we return, so we don't need
	 * anything smarter.
	 *
	 * Note that this will propagates any size changes to the decoded format.
	 *
	 * Default image_fmt to the codec's most common bit-depth (8-bit
	 * 4:2:0) rather than ANY. With ANY, all variants of a codec's
	 * decoded formats enumerate at S_FMT-CAPTURE time, and userspace
	 * codecs (gst's v4l2 stateless plugin in particular) will fixate
	 * to the highest-precision format available — picking NV15
	 * (10-bit) for Profile 0 8-bit content, which then produces
	 * incorrect output. The per-frame check_source_change /
	 * get_image_fmt path upgrades to 10-bit when actual 10-bit
	 * content is detected.
	 */
	ctx->image_fmt = RKVDEC_IMG_FMT_420_8BIT;
	rkvdec_reset_decoded_fmt(ctx);

	/* Propagate colorspace information to capture. */
	cap_fmt = &ctx->decoded_fmt;
	cap_fmt->fmt.pix_mp.colorspace = f->fmt.pix_mp.colorspace;
	cap_fmt->fmt.pix_mp.xfer_func = f->fmt.pix_mp.xfer_func;
	cap_fmt->fmt.pix_mp.ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
	cap_fmt->fmt.pix_mp.quantization = f->fmt.pix_mp.quantization;

	/* Enable format specific queue features */
	vq->subsystem_flags |= desc->subsystem_flags;

	return 0;
}

static int rkvdec_g_output_fmt(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct rkvdec_ctx *ctx = file_to_rkvdec_ctx(file);

	*f = ctx->coded_fmt;
	return 0;
}

static int rkvdec_g_capture_fmt(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rkvdec_ctx *ctx = file_to_rkvdec_ctx(file);

	*f = ctx->decoded_fmt;
	return 0;
}

static int rkvdec_subscribe_event(struct v4l2_fh *fh,
				  const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	default:
		return v4l2_ctrl_subscribe_event(fh, sub);
	}
}

static int rkvdec_enum_output_fmt(struct file *file, void *priv,
				  struct v4l2_fmtdesc *f)
{
	struct rkvdec_ctx *ctx = file_to_rkvdec_ctx(file);
	const struct rkvdec_coded_fmt_desc *desc;

	desc = rkvdec_enum_coded_fmt_desc(ctx, f->index);
	if (!desc)
		return -EINVAL;

	f->pixelformat = desc->fourcc;
	return 0;
}

static int rkvdec_enum_capture_fmt(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct rkvdec_ctx *ctx = file_to_rkvdec_ctx(file);
	u32 fourcc;

	fourcc = rkvdec_enum_decoded_fmt(ctx, f->index, ctx->image_fmt);
	if (!fourcc)
		return -EINVAL;

	f->pixelformat = fourcc;
	return 0;
}

static const struct v4l2_ioctl_ops rkvdec_ioctl_ops = {
	.vidioc_querycap = rkvdec_querycap,
	.vidioc_enum_framesizes = rkvdec_enum_framesizes,

	.vidioc_try_fmt_vid_cap_mplane = rkvdec_try_capture_fmt,
	.vidioc_try_fmt_vid_out_mplane = rkvdec_try_output_fmt,
	.vidioc_s_fmt_vid_out_mplane = rkvdec_s_output_fmt,
	.vidioc_s_fmt_vid_cap_mplane = rkvdec_s_capture_fmt,
	.vidioc_g_fmt_vid_out_mplane = rkvdec_g_output_fmt,
	.vidioc_g_fmt_vid_cap_mplane = rkvdec_g_capture_fmt,
	.vidioc_enum_fmt_vid_out = rkvdec_enum_output_fmt,
	.vidioc_enum_fmt_vid_cap = rkvdec_enum_capture_fmt,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = rkvdec_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_decoder_cmd = v4l2_m2m_ioctl_stateless_decoder_cmd,
	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_stateless_try_decoder_cmd,
};

static int rkvdec_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
			      unsigned int *num_planes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct rkvdec_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_format *f;
	u32 min_capt;
	unsigned int i;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		f = &ctx->coded_fmt;
	else
		f = &ctx->decoded_fmt;

	if (*num_planes) {
		if (*num_planes != f->fmt.pix_mp.num_planes)
			return -EINVAL;

		for (i = 0; i < f->fmt.pix_mp.num_planes; i++) {
			if (sizes[i] < f->fmt.pix_mp.plane_fmt[i].sizeimage)
				return -EINVAL;
		}
	} else {
		*num_planes = f->fmt.pix_mp.num_planes;
		for (i = 0; i < f->fmt.pix_mp.num_planes; i++)
			sizes[i] = f->fmt.pix_mp.plane_fmt[i].sizeimage;
	}

	/* Per-codec min CAPTURE count on the INITIAL allocation only
	 * (when the queue is empty).  gst v4l2codecs calls CREATE_BUFS in a
	 * loop adding one buffer at a time; only the first call has
	 * vq->num_buffers == 0.  Bumping later calls would make vb2 allocate
	 * too many on every call.
	 *
	 * Without enough buffers, gst recycles vb2 buffers faster than the
	 * codec retires reference frames, our reference-timestamp lookup
	 * falls through to the OUTPUT-buffer fallback, the HW reads garbage,
	 * and the IP core times out (LINK_STA_INT bit 2 = core_timeout). */
	if (V4L2_TYPE_IS_CAPTURE(vq->type) && vb2_get_num_buffers(vq) == 0) {
		switch (ctx->coded_fmt.fmt.pix_mp.pixelformat) {
		case V4L2_PIX_FMT_AV1_FRAME:  min_capt = 12; break;
		case V4L2_PIX_FMT_VP9_FRAME:  min_capt = 12; break;
		default:                       min_capt = 0;  break;
		}
		if (min_capt && *num_buffers < min_capt)
			*num_buffers = min_capt;
	}

	return 0;
}

static int rkvdec_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct rkvdec_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_format *f;
	unsigned int i;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		f = &ctx->coded_fmt;
	else
		f = &ctx->decoded_fmt;

	for (i = 0; i < f->fmt.pix_mp.num_planes; ++i) {
		u32 sizeimage = f->fmt.pix_mp.plane_fmt[i].sizeimage;

		if (vb2_plane_size(vb, i) < sizeimage)
			return -EINVAL;
	}

	/*
	 * Buffer's bytesused must be written by driver for CAPTURE buffers.
	 * (for OUTPUT buffers, if userspace passes 0 bytesused, v4l2-core sets
	 * it to buffer length).
	 */
	if (V4L2_TYPE_IS_CAPTURE(vq->type))
		vb2_set_plane_payload(vb, 0, f->fmt.pix_mp.plane_fmt[0].sizeimage);

	return 0;
}

static void rkvdec_buf_queue(struct vb2_buffer *vb)
{
	struct rkvdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int rkvdec_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

static void rkvdec_buf_request_complete(struct vb2_buffer *vb)
{
	struct rkvdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_hdl);
}

static int rkvdec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct rkvdec_ctx *ctx = vb2_get_drv_priv(q);
	const struct rkvdec_coded_fmt_desc *desc;
	int ret = 0;

	/*
	 * Queue (re)start may (re)allocate buffers with new IOVAs (e.g. after a
	 * resolution change / REQBUFS cycle), so force one TLB flush on the
	 * next kick. Steady-state decode leaves this clear (flush-only-after-
	 * restore throughput fix).
	 */
	ctx->dev->iommu_needs_flush = true;

	if (V4L2_TYPE_IS_CAPTURE(q->type)) {
		ctx->dev->accum_start_stream_cap++;
		return 0;
	}
	ctx->dev->accum_start_stream_out++;

	/*
	 * 2026-05-30 Round 2.4 retry + 2.5 extension: PM-guarded busy
	 * probe + link-register snapshot at start_streaming entry.
	 *
	 * Round 2.4 retry showed device is autosuspended at this boundary
	 * on safe content (susp_at_start always == stream count). So we
	 * force-resume here too (Round 2.5 pattern) — gives us session-
	 * ENTRY HW state to compare against the session-EXIT snapshot
	 * taken at vp9_stop. On safe content both should be clean and
	 * identical. On fp-tile content (future test) any divergence
	 * would localise where residue accumulates.
	 *
	 * accum_suspended_at_start now tracks "needed-to-resume" rather
	 * than "couldn't access" because resume_and_get force-resumes.
	 */
	if (ctx->dev->link) {
		int ret = pm_runtime_resume_and_get(ctx->dev->dev);

		if (!ret) {
			u32 dec_enable = readl_relaxed(ctx->dev->link + 0x040);
			u32 link_cfg   = readl_relaxed(ctx->dev->link + 0x004);
			u32 link_mode  = readl_relaxed(ctx->dev->link + 0x008);
			u32 link_dnum  = readl_relaxed(ctx->dev->link + 0x010);
			u32 link_tnum  = readl_relaxed(ctx->dev->link + 0x014);
			u32 link_en    = readl_relaxed(ctx->dev->link + 0x018);
			u32 int_en     = readl_relaxed(ctx->dev->link + 0x048);
			u32 sta_int    = readl_relaxed(ctx->dev->link + 0x04c);
			u32 ip_en      = readl_relaxed(ctx->dev->link + 0x058);

			if (dec_enable & VDPU383_DEC_E_BIT)
				ctx->dev->accum_busy_at_start++;

			dev_info(ctx->dev->dev,
				 "link@start_stream: dec_en=0x%08x cfg=0x%08x mode=0x%08x dnum=0x%08x tnum=0x%08x link_en=0x%08x int_en=0x%08x sta=0x%08x ip_en=0x%08x\n",
				 dec_enable, link_cfg, link_mode, link_dnum,
				 link_tnum, link_en, int_en, sta_int, ip_en);

			/*
			 * R36 (2026-06-01) — per-session warmup. We hold a
			 * runtime ref via resume_and_get above, so HW is
			 * powered & clocked; safe to fire the warmup
			 * descriptor here. Fires once per gst session.
			 */
			/* No vdpu383_variant check here — vdpu383_variant is
			 * static and defined later in this TU. Rely on the
			 * warmup buffer being allocated, which probe only
			 * does on VDPU383 anyway. */
			if (r36_warmup_on_start_streaming &&
			    ctx->dev->link && ctx->dev->rk3576_warmup_cpu) {
				int rc = rkvdec_rk3576_warmup_run(
					ctx->dev->link,
					ctx->dev->rk3576_warmup_dma);
				ctx->dev->r36_start_warmups++;
				if (rc == 0)
					ctx->dev->r36_start_warmups_ok++;
				else if (rc == -EIO)
					ctx->dev->r36_start_warmups_eio++;
				else
					ctx->dev->r36_start_warmups_err++;
				if (rc && rc != -EIO)
					dev_warn_ratelimited(ctx->dev->dev,
						"R36 warmup@start_streaming: %d\n",
						rc);
			}

			pm_runtime_put_autosuspend(ctx->dev->dev);
		} else {
			ctx->dev->accum_suspended_at_start++;
		}
	}

	desc = ctx->coded_fmt_desc;
	if (WARN_ON(!desc))
		return -EINVAL;

	/* Step A telemetry: reset per-session counters. */
	ctx->dev->telem_irq_done_clean = 0;
	ctx->dev->telem_irq_done_err = 0;
	ctx->dev->telem_irq_nowb = 0;
	ctx->dev->telem_silent_completion = 0;
	ctx->dev->telem_silent_clean_wb = 0;
	ctx->dev->telem_silent_err_wb = 0;
	ctx->dev->telem_silent_dec_only = 0;
	ctx->dev->telem_silent_no_wb = 0;
	ctx->dev->telem_real_timeout = 0;
	ctx->dev->telem_irq_spurious = 0;
	ctx->dev->telem_ref_lookup_total = 0;
	ctx->dev->telem_ref_lookup_fallback = 0;

	if (desc->ops->start)
		ret = desc->ops->start(ctx);

	return ret;
}

static void rkvdec_queue_cleanup(struct vb2_queue *vq, u32 state)
{
	struct rkvdec_ctx *ctx = vb2_get_drv_priv(vq);

	while (true) {
		struct vb2_v4l2_buffer *vbuf;

		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (!vbuf)
			break;

		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req,
					   &ctx->ctrl_hdl);
		v4l2_m2m_buf_done(vbuf, state);
	}
}

static void rkvdec_stop_streaming(struct vb2_queue *q)
{
	struct rkvdec_ctx *ctx = vb2_get_drv_priv(q);

	if (V4L2_TYPE_IS_CAPTURE(q->type))
		ctx->dev->accum_stop_stream_cap++;

	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		const struct rkvdec_coded_fmt_desc *desc = ctx->coded_fmt_desc;

		ctx->dev->accum_stop_stream_out++;

		/*
		 * 2026-05-30 Round 2.4 retry (PM-guarded): HW-busy-at-stop
		 * probe. Same pattern as start_streaming above.
		 */
		if (ctx->dev->link) {
			if (pm_runtime_get_if_in_use(ctx->dev->dev) > 0) {
				if (readl_relaxed(ctx->dev->link +
						  VDPU383_LINK_DEC_ENABLE) &
				    VDPU383_DEC_E_BIT)
					ctx->dev->accum_busy_at_stop_out++;
				pm_runtime_put(ctx->dev->dev);
			} else {
				ctx->dev->accum_suspended_at_stop_out++;
			}
		}

		if (WARN_ON(!desc))
			return;

		/* Step A telemetry: dump completion-path counts for the
		 * session that just ended. irq_spurious counts IRQs that
		 * the outer dispatcher discarded because LINK_DEC_NUM
		 * didn't advance from expected - if this is high relative
		 * to silent, HW IS firing IRQs per task but we're
		 * filtering them out as spurious. */
		dev_info(ctx->dev->dev,
			 "telem: irq_clean=%u irq_err=%u irq_nowb=%u silent=%u real_to=%u spurious=%u total=%u\n",
			 ctx->dev->telem_irq_done_clean,
			 ctx->dev->telem_irq_done_err,
			 ctx->dev->telem_irq_nowb,
			 ctx->dev->telem_silent_completion,
			 ctx->dev->telem_real_timeout,
			 ctx->dev->telem_irq_spurious,
			 ctx->dev->telem_irq_done_clean +
			 ctx->dev->telem_irq_done_err +
			 ctx->dev->telem_irq_nowb +
			 ctx->dev->telem_silent_completion +
			 ctx->dev->telem_real_timeout);
		dev_info(ctx->dev->dev,
			 "telem: ref_lookup_total=%u ref_lookup_fallback=%u\n",
			 ctx->dev->telem_ref_lookup_total,
			 ctx->dev->telem_ref_lookup_fallback);
		dev_info(ctx->dev->dev,
			 "telem: silent_clean_wb=%u silent_err_wb=%u silent_dec_only=%u silent_no_wb=%u\n",
			 ctx->dev->telem_silent_clean_wb,
			 ctx->dev->telem_silent_err_wb,
			 ctx->dev->telem_silent_dec_only,
			 ctx->dev->telem_silent_no_wb);
		dev_info(ctx->dev->dev,
			 "telem: r35_resume_total=%u r35_resume_warmups=%u (ok=%u eio=%u err=%u) r36_start_warmups=%u (ok=%u eio=%u err=%u)\n",
			 ctx->dev->r35_resume_total_calls,
			 ctx->dev->r35_resume_warmups,
			 ctx->dev->r35_resume_warmups_ok,
			 ctx->dev->r35_resume_warmups_eio,
			 ctx->dev->r35_resume_warmups_err,
			 ctx->dev->r36_start_warmups,
			 ctx->dev->r36_start_warmups_ok,
			 ctx->dev->r36_start_warmups_eio,
			 ctx->dev->r36_start_warmups_err);

		/*
		 * 2026-05-30 Round 2.2 accumulation-bug instrumentation.
		 * These counts are device-level (NOT reset per session) so
		 * we can watch for imbalance across vectors in one Fluster
		 * batch. Crash trigger is documented at 16 different 14-
		 * resize vectors → look for any pair whose delta widens
		 * non-linearly with vector index.
		 */
		dev_info(ctx->dev->dev,
			 "accum: open=%u rel=%u stream_out=%u/%u stream_cap=%u/%u vp9=%u/%u rcb=%u/%u rebuild=%u busy[start=%u stop_out=%u vp9_stop=%u] susp[start=%u stop_out=%u vp9_stop=%u]\n",
			 ctx->dev->accum_open,
			 ctx->dev->accum_release,
			 ctx->dev->accum_start_stream_out,
			 ctx->dev->accum_stop_stream_out,
			 ctx->dev->accum_start_stream_cap,
			 ctx->dev->accum_stop_stream_cap,
			 ctx->dev->accum_vp9_start,
			 ctx->dev->accum_vp9_stop,
			 ctx->dev->accum_rcb_alloc,
			 ctx->dev->accum_rcb_free,
			 ctx->dev->accum_rcb_rebuild,
			 ctx->dev->accum_busy_at_start,
			 ctx->dev->accum_busy_at_stop_out,
			 ctx->dev->accum_busy_at_vp9_stop,
			 ctx->dev->accum_suspended_at_start,
			 ctx->dev->accum_suspended_at_stop_out,
			 ctx->dev->accum_suspended_at_vp9_stop);

		/* vp9_time diagnostic: flush any partial HW-timing accumulation
		 * (streams shorter than the 100-frame print threshold, or with
		 * many show-existing/alt-ref frames that don't fire a decode IRQ,
		 * otherwise never report). */
		if (vp9_time && ctx->dev->vp9_dec_ns_cnt) {
			u32 n = ctx->dev->vp9_dec_ns_cnt;

			dev_info(ctx->dev->dev,
				 "vp9_time flush: HWdecode=%llu us IRQlat=%llu us total=%llu us/frame over %u decodes\n",
				 ctx->dev->vp9_dec_ns_sum / n / 1000,
				 ctx->dev->vp9_irq_ns_sum / n / 1000,
				 (ctx->dev->vp9_dec_ns_sum +
				  ctx->dev->vp9_irq_ns_sum) / n / 1000, n);
			ctx->dev->vp9_dec_ns_sum = 0;
			ctx->dev->vp9_irq_ns_sum = 0;
			ctx->dev->vp9_dec_ns_cnt = 0;
		}

		/*
		 * vdpu383-submit Phase 1a: drain any pending link-completion
		 * reap before the codec stop() tears down ctx->link_table, so a
		 * worker reap can't touch a freed table. No-op when the worker
		 * was never used (submit_mode!=2). The watchdog is
		 * cancel_delayed_work_sync'd at remove; here job_abort has
		 * already forced inflight to error-complete via the m2m path.
		 */
		if (ctx->dev->link_worker)
			kthread_flush_work(&ctx->dev->link_reap_work);

		if (desc->ops->stop)
			desc->ops->stop(ctx);

		vb2_wait_for_all_buffers(q);

		rkvdec_free_rcb(ctx);
	}

	rkvdec_queue_cleanup(q, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops rkvdec_queue_ops = {
	.queue_setup = rkvdec_queue_setup,
	.buf_prepare = rkvdec_buf_prepare,
	.buf_queue = rkvdec_buf_queue,
	.buf_out_validate = rkvdec_buf_out_validate,
	.buf_request_complete = rkvdec_buf_request_complete,
	.start_streaming = rkvdec_start_streaming,
	.stop_streaming = rkvdec_stop_streaming,
};

static int rkvdec_request_validate(struct media_request *req)
{
	unsigned int count;

	count = vb2_request_buffer_cnt(req);
	if (!count)
		return -ENOENT;
	else if (count > 1)
		return -EINVAL;

	return vb2_request_validate(req);
}

static const struct media_device_ops rkvdec_media_ops = {
	.req_validate = rkvdec_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static void rkvdec_job_finish_no_pm(struct rkvdec_ctx *ctx,
				    enum vb2_buffer_state result)
{
	/*
	 * Phase 3 v0.3 step 2.4: in link mode vp9_run detaches src/dst
	 * from m2m's rdy_queue and parks them in ctx->inflight, so the
	 * peek-via-m2m_next_*_buf path no longer applies. Pop the oldest
	 * inflight entry, call codec done() on those specific buffers,
	 * mark them done back to userspace, then job_finish.
	 *
	 * Both the IRQ handler and the watchdog path land here, so the
	 * watchdog fire (e.g. silent_completion recovery) sees the same
	 * inflight ring the IRQ would have. Without this the watchdog
	 * would call v4l2_m2m_buf_done_and_job_finish on an empty rdy_queue
	 * and the inflight-pinned buffers would never be returned to
	 * userspace.
	 */
	if (rkvdec_link_mode && ctx->link_table) {
		/*
		 * Watchdog/error path for link mode: drain ALL inflight
		 * entries with `result` (typically VB2_BUF_STATE_ERROR for
		 * a real timeout, or VB2_BUF_STATE_DONE for silent
		 * completion). With batched fill (step 2.6) the watchdog
		 * fires once per kick — which may cover multiple tasks —
		 * so we have to mop up everything that was in flight.
		 *
		 * The IRQ-side completion path does its own per-task
		 * drain inline in vdpu383_irq_handler and does not enter
		 * rkvdec_job_finish_no_pm.
		 *
		 * Empty ring means the IRQ side already drained and
		 * job_finish'd; skip our job_finish to avoid m2m
		 * dispatching a new device_run on an empty rdy_queue.
		 */
		struct rkvdec_link_inflight e;
		bool any = false;

		while ((e = rkvdec_inflight_pop(ctx)).src && e.dst) {
			if (ctx->coded_fmt_desc->ops->done)
				ctx->coded_fmt_desc->ops->done(
					ctx, e.src, e.dst, result);
			v4l2_m2m_buf_done(e.src, result);
			v4l2_m2m_buf_done(e.dst, result);
			any = true;
		}
		if (!any) {
			/*
			 * 2026-05-31 link-mode-tile-reject fix: in link mode
			 * an empty inflight ring at job_finish time has two
			 * very different causes depending on `result`:
			 *
			 *  - result == DONE: the IRQ handler already drained
			 *    the ring inline and called job_finish. Skipping
			 *    avoids m2m double-dispatch. (Original behavior.)
			 *
			 *  - result == ERROR: the device_run path called us
			 *    because vp9_run returned -EINVAL BEFORE pushing
			 *    anything to inflight (e.g. the Round 3 tile-
			 *    content reject). m2m's rdy_queue still holds the
			 *    current src/dst pair; if we skip job_finish here,
			 *    m2m waits forever for an IRQ that never fires and
			 *    gst-launch wedges in v4l2_m2m_dqbuf D-state.
			 *    Fall through to the single-shot cleanup path so
			 *    the current m2m task is marked ERROR and m2m can
			 *    dispatch the next device_run.
			 */
			if (result == VB2_BUF_STATE_DONE) {
				pr_warn_ratelimited("rkvdec: job_finish but inflight ring empty (skip)\n");
				return;
			}
			/* result == ERROR (or any non-DONE state):
			 * fall through to single-shot cleanup below */
		} else {
			v4l2_m2m_job_finish(ctx->dev->m2m_dev,
					    ctx->fh.m2m_ctx);
			return;
		}
	}

	if (ctx->coded_fmt_desc->ops->done) {
		struct vb2_v4l2_buffer *src_buf, *dst_buf;

		src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
		dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
		ctx->coded_fmt_desc->ops->done(ctx, src_buf, dst_buf, result);
	}

	v4l2_m2m_buf_done_and_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx,
					 result);
}

static void rkvdec_job_finish(struct rkvdec_ctx *ctx,
			      enum vb2_buffer_state result)
{
	struct rkvdec_dev *rkvdec = ctx->dev;

	pm_runtime_put_autosuspend(rkvdec->dev);
	rkvdec_job_finish_no_pm(ctx, result);
}

/*
 * Link-mode completion reap (LINK_MODE_PORT_DESIGN_2026-06-01 §3). The
 * watchdog poll (sole completion owner) calls this once HW has finished.
 * Unlike single-shot's rkvdec_job_finish, the m2m job was ALREADY finished
 * at submit time (vp9_run calls v4l2_m2m_job_finish), so here we only
 * return the parked buffers to userspace, drop one pm reference per task
 * (paired with the pm_get rkvdec_device_run did for each submit), and
 * re-check job_ready so m2m dispatches the next frame.
 *
 * At depth=1 the ring holds exactly one task, so draining the whole ring
 * with the single computed `state` is correct. Depth>1 will need per-slot
 * writeback discrimination instead — deferred to the depth-raise phase.
 */
static void rkvdec_link_reap(struct rkvdec_ctx *ctx,
			     enum vb2_buffer_state state)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_link_inflight e;
	bool any = false;

	while ((e = rkvdec_inflight_pop(ctx)).src && e.dst) {
		if (ctx->coded_fmt_desc->ops->done)
			ctx->coded_fmt_desc->ops->done(ctx, e.src, e.dst,
						       state);
		v4l2_m2m_buf_done(e.src, state);
		v4l2_m2m_buf_done(e.dst, state);
		pm_runtime_put_autosuspend(rkvdec->dev);
		any = true;
	}

	/* Wake m2m so it re-evaluates job_ready and dispatches the next
	 * device_run now that a slot has freed. */
	if (any)
		v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
}

/*
 * BSP-faithful writeback reap (LINK_MODE_PORT_DESIGN_2026-06-01, 2026-06-01
 * BSP .104 capture). HW fires the link IRQ and writes each task's status
 * into that descriptor's tb_reg_int slot (observed on BSP: irq 0x48=0x5 /
 * 0x4c=0x1, writeback 0xf0000001). Mirror rkvdec2_link_try_dequeue:
 * task_done = tb_reg[tb_reg_int] != 0; err = writeback & err_mask.
 *
 * Reap every in-flight task whose OWN slot has a non-zero writeback,
 * oldest first, stopping at the first task HW hasn't finished. Reading
 * the per-slot writeback (not slot 0, not dec_num) is the fix: the ring
 * rotates, so the old slot-0 read always saw 0 ("silent_no_wb").
 *
 * Called from the threaded IRQ handler and the watchdog; both may run
 * done()/buf_done (sleeping ops) in process context. Returns the number
 * of tasks reaped.
 */
/*
 * Atomically pop the oldest in-flight task IFF HW has written its slot's
 * descriptor writeback. The writeback read + the pop happen under the
 * inflight spinlock so the threaded IRQ and the watchdog can never race:
 * each completed entry is popped exactly once with its OWN writeback state.
 * (The earlier peek-then-pop split was the concurrency hazard that wedged
 * the box on 2026-06-01.) Returns true and fills *out/*state on success.
 */
static bool rkvdec_link_pop_if_done(struct rkvdec_ctx *ctx,
				    struct rkvdec_link_inflight *out,
				    enum vb2_buffer_state *state)
{
	unsigned long flags;
	bool got = false;

	spin_lock_irqsave(&ctx->inflight_lock, flags);
	if (ctx->inflight_tail != ctx->inflight_head) {
		u32 slot = ctx->inflight[ctx->inflight_tail].slot;
		u32 wb = rkvdec_link_read_task_status(ctx->link_table, slot);

		/*
		 * Only reap CLEAN completions. A writeback with error bits
		 * (wb & err_mask) is LEFT in flight so the watchdog times out
		 * and does a quiesced reset + RESEND of the task — BSP recovers
		 * error tasks by resend, not by handing userspace a bad frame.
		 * (yt INTER/alt-ref intermittently err-writebacks; without this
		 * the err frame is reaped ERROR -> gst flow-error -5.)
		 * wb==0 / sentinel = not finished yet -> also left in flight.
		 *
		 * NB (2026-06-07): bit 2 (0xf0000004) writebacks are NOT benign in
		 * link depth>1 — they are PARTIAL frames the mid-decode append
		 * disturbed; the watchdog reset+RESEND re-decodes them correctly.
		 * So bit 2 stays in the error test (reaping it hands a partial
		 * frame to userspace). The real fix is to avoid the append entirely
		 * (batch-only), not to reap partials.
		 */
		if (wb != 0 && wb != 0xffffffffu &&
		    !(wb & ctx->link_table->info->err_mask)) {
			*out = ctx->inflight[ctx->inflight_tail];
			*state = VB2_BUF_STATE_DONE;
			ctx->inflight_tail = (ctx->inflight_tail + 1) %
					     RKVDEC_LINK_INFLIGHT_MAX;
			got = true;
		}
	}
	spin_unlock_irqrestore(&ctx->inflight_lock, flags);
	return got;
}

static u32 rkvdec_link_reap_writeback(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_link_inflight e;
	enum vb2_buffer_state state;
	u32 reaped = 0;

	while (rkvdec_link_pop_if_done(ctx, &e, &state)) {
		if (!e.src || !e.dst)
			break;
		if (ctx->coded_fmt_desc->ops->done)
			ctx->coded_fmt_desc->ops->done(ctx, e.src, e.dst, state);
		v4l2_m2m_buf_done(e.src, state);
		v4l2_m2m_buf_done(e.dst, state);
		pm_runtime_put_autosuspend(rkvdec->dev);
		reaped++;
	}

	if (reaped) {
		u32 left = rkvdec_inflight_depth(ctx);

		ctx->link_resets = 0;	/* progress -> refresh reset budget */
		cancel_delayed_work(&rkvdec->watchdog_work);
		/*
		 * 2026-06-07 depth>1: if tasks remain in flight (e.g. appended
		 * frames whose completion hasn't landed yet), keep a fallback
		 * watchdog so a missed IRQ on them can't strand the ring. Without
		 * this the cancel above left the remainder with no reaper.
		 */
		if (left) {
			ctx->watchdog_poll_count = 0;
			schedule_delayed_work(&rkvdec->watchdog_work,
				msecs_to_jiffies(RKVDEC_WATCHDOG_POLL_MS));
		}
		pr_debug("rkvdec link-reap: reaped=%u left=%u\n", reaped, left);
		v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
	}
	return reaped;
}

/*
 * vdpu383-submit PLAN Phase 1a (rkvdec_submit_mode=2): kthread-worker link
 * completion. The hardirq top-half (rkvdec_irq_top) acks the link IRQ and
 * kthread_queue_work()s this; here, in the resident worker's process context,
 * we run the (sleeping) per-slot writeback reap. This relocates the reap off
 * the threaded-IRQ path — bit-for-bit the same reap the threaded handler did,
 * just owned by a resident kthread. It is the first step of the MPP-style
 * kthread runtime that removes the per-frame threaded-IRQ ms-scale wake on the
 * completion path (the throughput ceiling from SESSION_2026-06-07_h264_linkmode).
 *
 * Single owner: link_ctx is the stashed completion-owner ctx (single-stream
 * endpoint — one ctx in flight at a time, same assumption the watchdog and
 * threaded handler already make).
 */
static void rkvdec_link_reap_work_fn(struct kthread_work *work)
{
	struct rkvdec_dev *rkvdec =
		container_of(work, struct rkvdec_dev, link_reap_work);
	struct rkvdec_ctx *ctx = rkvdec->link_ctx;
	unsigned long deadline;
	u32 spins = 0;

	if (!ctx || !ctx->link_table)
		return;

	if (!rkvdec_link_poll) {
		/* Phase 1a: one reap per IRQ wake, then sleep. */
		rkvdec_link_reap_writeback(ctx);
		return;
	}

	/*
	 * Phase 1b: keep the worker hot. rkvdec_link_reap_writeback reaps every
	 * completed slot AND inline-feeds the next frame (its tail
	 * v4l2_m2m_try_schedule runs device_run synchronously in this context).
	 * Rather than return after one reap and pay the wake-from-idle latency
	 * before the next frame, poll the per-slot writeback until HW drains —
	 * so HW is re-armed the instant a slot frees. Bounded by a 250 ms
	 * deadline + cpu_relax/cond_resched: a genuinely stuck slot exits the
	 * loop and the fallback watchdog (armed by run() under lineirq) recovers
	 * it, and other tasks (gst) still get the CPU.
	 */
	deadline = jiffies + msecs_to_jiffies(250);
	do {
		u32 reaped = rkvdec_link_reap_writeback(ctx);

		if (rkvdec_inflight_depth(ctx) == 0)
			break;		/* nothing in flight -> truly idle */
		if (reaped) {
			spins = 0;
		} else {
			cpu_relax();
			if ((++spins & 0x3ff) == 0)
				cond_resched();
		}
	} while (time_before(jiffies, deadline));
}

static void rkvdec_iommu_restore(struct rkvdec_dev *rkvdec);

/*
 * BSP-model port (NEXT_SESSION_BSP_MODEL_PORT_2026-06-03): wedge-safe
 * link reset, mirroring rkvdec2_link_reset. Order is what makes it safe
 * vs the earlier mid-DMA soft-reset that wedged the box:
 *   disable_irq -> STOP HW (IP soft-reset) -> re-attach IOMMU (now that
 *   HW is quiesced) -> enable_irq.
 * Runs in watchdog (process) context, so disable_irq + udelay are fine.
 * Leaves HW ready for a fresh bootstrap kick (LINK_EN=0, status clear,
 * INT_EN re-armed, ip_en restored).
 */
static void rkvdec_link_quiesced_reset(struct rkvdec_dev *rkvdec,
				       struct rkvdec_ctx *ctx)
{
	void __iomem *link = rkvdec->link;
	u32 ip_en;
	int poll;

	disable_irq(rkvdec->irq);

	/* STOP HW: IP soft-reset (HW is potentially mid-DMA; this halts it
	 * BEFORE we touch the IOMMU). */
	ip_en = readl(link + VDPU383_LINK_IP_ENABLE);
	writel(ip_en | BIT(15), link + VDPU383_LINK_IP_ENABLE);
	writel(BIT(0), link + 0x44);
	for (poll = 0; poll < 50; poll++) {
		if (readl(link + VDPU383_LINK_STA_INT) & BIT(11))
			break;
		udelay(10);
	}
	writel(0, link + 0x18);				/* LINK_EN = 0 */
	writel(0xffff0000u, link + 0x48);
	writel(0xffff0000u, link + 0x4c);
	writel(ip_en, link + VDPU383_LINK_IP_ENABLE);	/* restore */

	/* HW stopped -> safe to detach/reattach the IOMMU domain. */
	rkvdec_iommu_restore(rkvdec);

	if (ctx->link_table)
		ctx->link_table->expected_dec_num = 0;

	/* Re-arm the frame-done IRQ for the resend. */
	writel(FIELD_PREP_WM16(VDPU383_INT_EN_IRQ, VDPU383_INT_EN_IRQ),
	       link + VDPU383_LINK_INT_EN);

	enable_irq(rkvdec->irq);
}

void rkvdec_run_preamble(struct rkvdec_ctx *ctx, struct rkvdec_run *run)
{
	struct media_request *src_req;

	memset(run, 0, sizeof(*run));

	run->bufs.src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run->bufs.dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	/* Apply request(s) controls if needed. */
	src_req = run->bufs.src->vb2_buf.req_obj.req;
	if (src_req)
		v4l2_ctrl_request_setup(src_req, &ctx->ctrl_hdl);

	v4l2_m2m_buf_copy_metadata(run->bufs.src, run->bufs.dst);
}

void rkvdec_run_postamble(struct rkvdec_ctx *ctx, struct rkvdec_run *run)
{
	struct media_request *src_req = run->bufs.src->vb2_buf.req_obj.req;

	if (src_req)
		v4l2_ctrl_request_complete(src_req, &ctx->ctrl_hdl);
}

void rkvdec_quirks_disable_qos(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	u32 reg;

	/* Set undocumented swreg_block_gating_e field */
	reg = readl(rkvdec->regs + RKVDEC_REG_QOS_CTRL);
	reg &= GENMASK(31, 16);
	reg |= 0xEFFF;
	writel(reg, rkvdec->regs + RKVDEC_REG_QOS_CTRL);
}

void rkvdec_memcpy_toio(void __iomem *dst, void *src, size_t len)
{
#ifdef CONFIG_ARM64
	__iowrite32_copy(dst, src, len / 4);
#else
	memcpy_toio(dst, src, len);
#endif
}

void rkvdec_schedule_watchdog(struct rkvdec_dev *rkvdec, u32 timeout_threshold)
{
	/* Set watchdog at 2 times the hardware timeout threshold */
	u32 watchdog_time;
	unsigned long axi_rate = clk_get_rate(rkvdec->axi_clk);

	if (axi_rate)
		watchdog_time = 2 * div_u64(1000 * (u64)timeout_threshold, axi_rate);
	else
		watchdog_time = 2000;

	schedule_delayed_work(&rkvdec->watchdog_work, msecs_to_jiffies(watchdog_time));
}

/* Step A v2: schedule the watchdog as a short silent-completion poll
 * (10 ms) rather than as the full timeout. Used by vp9_run as the
 * initial schedule (so the FIRST fire is the fast detector, not the
 * slow timeout) and by the watchdog handler's early-return path when
 * HW is still mid-decode. */
void rkvdec_schedule_watchdog_poll(struct rkvdec_dev *rkvdec)
{
	schedule_delayed_work(&rkvdec->watchdog_work,
			      msecs_to_jiffies(RKVDEC_WATCHDOG_POLL_MS));
}

/*
 * Lead 2 (§7c) NEGATIVE 2026-06-02: pinning decoder clocks to max (BSP
 * CLK_MODE_ADVANCED analog) made link mode ALL-BAD both per-frame and
 * once-per-session — high clock is harmful to the link state machine on
 * this IP, not the cure. Helper removed; see vp9_start for the note.
 */

/*
 * Silent IOMMU fault handler — replaces rk_iommu's noisy default so
 * dmesg has room for our diagnostic prints. Logs only the first fault
 * and a rate-limited summary.
 */
static int rkvdec_silent_iommu_fault(struct iommu_domain *domain,
				     struct device *dev, unsigned long iova,
				     int flags, void *token)
{
	static unsigned long fault_count;
	fault_count++;
	pr_warn_ratelimited("rkvdec-silent-iommu: fault #%lu iova=0x%lx flags=0x%x\n",
			    fault_count, iova, flags);
	/* Return 0 to indicate fault handled and prevent retry storm. */
	return 0;
}

/*
 * Workaround for Rockchip IOMMU "fetch DTE time limit" silicon race
 * (RV1126/RV1109/RK356X/RK3588 — almost certainly RK3576 too).
 *
 * Newer silicon hangs after 4 consecutive DTE fetches racing with a
 * CPU-side IOTLB zap, producing a Page-fault-at-iova=0 storm. Vendor
 * fix is to set BIT(31) of MMU_AUTO_GATING (offset 0x24) so the IOMMU
 * behaves like the older non-buggy version. The patch is in upstream
 * review by Sven Püschel / Simon Xue but not merged yet; we apply it
 * from our OOT driver per-run because runtime PM can power-cycle the
 * IOMMU and reset the bit. Cheap (two MMIO read+write per frame) and
 * idempotent.
 */
#define RKVDEC_IOMMU_AUTO_GATING_OFF   0x24
#define RKVDEC_IOMMU_DTE_LIMIT_DISABLE BIT(31)

static void rkvdec_apply_iommu_dte_workaround(struct rkvdec_dev *rkvdec)
{
	struct device_node *iommu_node;
	struct platform_device *iommu_pdev;
	int i;

	iommu_node = of_parse_phandle(rkvdec->dev->of_node, "iommus", 0);
	if (!iommu_node) {
		pr_info_once("rkvdec: DTE WA: no iommus phandle\n");
		return;
	}

	iommu_pdev = of_find_device_by_node(iommu_node);
	of_node_put(iommu_node);
	if (!iommu_pdev) {
		pr_info_once("rkvdec: DTE WA: no iommu pdev\n");
		return;
	}

	for (i = 0; i < 4; i++) {
		struct resource *res;
		void __iomem *base;
		u32 val_before, val_after;

		res = platform_get_resource(iommu_pdev, IORESOURCE_MEM, i);
		if (!res) {
			pr_info_once("rkvdec: DTE WA: no more resources (i=%d)\n", i);
			break;
		}

		base = ioremap(res->start, resource_size(res));
		if (!base) {
			pr_info_once("rkvdec: DTE WA: ioremap failed @ 0x%llx\n",
				     (u64)res->start);
			continue;
		}

		val_before = readl(base + RKVDEC_IOMMU_AUTO_GATING_OFF);
		writel(val_before | RKVDEC_IOMMU_DTE_LIMIT_DISABLE,
		       base + RKVDEC_IOMMU_AUTO_GATING_OFF);
		val_after = readl(base + RKVDEC_IOMMU_AUTO_GATING_OFF);
		pr_info_once("rkvdec: DTE WA @ 0x%llx: before=0x%08x after=0x%08x\n",
			     (u64)res->start, val_before, val_after);

		iounmap(base);
	}

	put_device(&iommu_pdev->dev);
}

/*
 * Bug-A dmaengine phase 3: one-shot test of PL330 memcpy against a real
 * V4L2 CAPTURE buffer's physical address (resolved via rkvdec's IOMMU
 * domain). HIGH RISK — the previous CPU-mapped memcpy attempts wedged
 * the kernel three times; this is a different path (PL330 hardware DMA
 * with no CPU mapping) but still touching V4L2 buffer memory.
 *
 * Strategy: 16-byte memcpy at offset 0 of the current src buffer into
 * itself (src=dst, no destructive write since we read+write the same
 * bytes). Tests address bridging without altering buffer contents.
 *
 * Runs once per module load and only when vp9_bug_a_phase >= 3.
 */
static void rkvdec_bug_a_phase3_test(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct vb2_v4l2_buffer *src_vb;
	dma_addr_t iova;
	phys_addr_t phys;
	struct iommu_domain *dom;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	enum dma_status status;
	int wait_ms = 100;

	if (vp9_bug_a_phase3_attempted)
		return;
	vp9_bug_a_phase3_attempted = true;

	if (!vp9_bug_a_chan) {
		dev_warn(rkvdec->dev, "bug-a phase 3: no DMA channel, aborting\n");
		return;
	}

	src_vb = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	if (!src_vb) {
		dev_warn(rkvdec->dev, "bug-a phase 3: no src buf, aborting\n");
		return;
	}

	iova = vb2_dma_contig_plane_dma_addr(&src_vb->vb2_buf, 0);
	dom = iommu_get_domain_for_dev(rkvdec->dev);
	if (!dom) {
		dev_warn(rkvdec->dev, "bug-a phase 3: no rkvdec iommu_domain, aborting\n");
		return;
	}
	phys = iommu_iova_to_phys(dom, iova);
	if (!phys) {
		dev_warn(rkvdec->dev, "bug-a phase 3: iommu_iova_to_phys(%pad) returned 0, aborting\n",
			 &iova);
		return;
	}

	dev_info(rkvdec->dev,
		 "bug-a phase 3: attempting 16-byte src=dst memcpy at phys=%pap (iova=%pad)\n",
		 &phys, &iova);

	tx = dmaengine_prep_dma_memcpy(vp9_bug_a_chan,
				       (dma_addr_t)phys, (dma_addr_t)phys,
				       16, DMA_PREP_INTERRUPT);
	if (!tx) {
		dev_warn(rkvdec->dev, "bug-a phase 3: prep_dma_memcpy returned NULL\n");
		return;
	}

	cookie = dmaengine_submit(tx);
	dma_async_issue_pending(vp9_bug_a_chan);

	while (wait_ms-- > 0) {
		status = dma_async_is_tx_complete(vp9_bug_a_chan, cookie,
						  NULL, NULL);
		if (status == DMA_COMPLETE)
			break;
		mdelay(1);
	}

	if (status == DMA_COMPLETE)
		dev_info(rkvdec->dev, "bug-a phase 3: PL330 memcpy on V4L2 buffer SUCCESS\n");
	else
		dev_warn(rkvdec->dev, "bug-a phase 3: PL330 memcpy did not complete (status=%d)\n",
			 status);
}

static void rkvdec_device_run(void *priv)
{
	struct rkvdec_ctx *ctx = priv;
	struct rkvdec_dev *rkvdec = ctx->dev;
	const struct rkvdec_coded_fmt_desc *desc = ctx->coded_fmt_desc;
	int ret;

	if (WARN_ON(!desc))
		return;

	ret = pm_runtime_resume_and_get(rkvdec->dev);
	if (ret < 0) {
		rkvdec_job_finish_no_pm(ctx, VB2_BUF_STATE_ERROR);
		return;
	}

	rkvdec_apply_iommu_dte_workaround(rkvdec);

	if (vp9_bug_a_phase >= 3)
		rkvdec_bug_a_phase3_test(ctx);

	if (!rkvdec_rcb_buf_validate_size(ctx)) {
		rkvdec->accum_rcb_rebuild++;
		rkvdec_free_rcb(ctx);

		ret = rkvdec_allocate_rcb(ctx,
					  ctx->decoded_fmt.fmt.pix_mp.width,
					  ctx->decoded_fmt.fmt.pix_mp.height,
					  ctx->dev->variant->rcb_sizes,
					  ctx->dev->variant->num_rcb_sizes);
		if (ret) {
			rkvdec_job_finish(ctx, VB2_BUF_STATE_ERROR);
			return;
		}
	}

	/* 2026-06-10: gated full IOMMU HW refresh (mpp_iommu_refresh equivalent). */
	if (iommu_refresh_per_decode) {
		struct iommu_domain *dom;

		rkvdec_iommu_restore(rkvdec);
		dom = iommu_get_domain_for_dev(rkvdec->dev);
		if (dom)
			iommu_flush_iotlb_all(dom);
	}

	ret = desc->ops->run(ctx);
	if (ret)
		rkvdec_job_finish(ctx, VB2_BUF_STATE_ERROR);
}

/*
 * Called by v4l2_m2m framework during STREAMOFF when an active job is
 * still in flight. Without this callback, vb2_wait_for_all_buffers in
 * our stop_streaming() path blocks forever waiting for a buffer that's
 * marked ACTIVE (handed to HW) but for which HW never fired the IRQ
 * (silent-fail HW state observed on tile_1x2_frame_parallel content).
 *
 * Force the current job to error completion: rkvdec_job_finish drains
 * inflight entries (link mode) AND marks the m2m current src/dst as
 * VB2_BUF_STATE_ERROR (single-shot mode via the m2m_buf_done_and_job_finish
 * path), unblocking vb2_wait_for_all_buffers.
 *
 * HW may still be processing the buffer when this runs, but our DMA-
 * coherent allocations remain mapped through end of streaming; HW won't
 * fault. The buffer content is meaningless either way (HW silently
 * failed), so marking ERROR is the right outcome.
 */
static void rkvdec_job_abort(void *priv)
{
	struct rkvdec_ctx *ctx = priv;

	rkvdec_job_finish(ctx, VB2_BUF_STATE_ERROR);
}

/*
 * Link-mode backpressure (LINK_MODE_PORT_DESIGN_2026-06-01 §3). m2m has
 * already verified both queues have buffers ready before calling this;
 * we only add the in-flight gate. device_run job_finishes at submit, so
 * without this gate m2m would dispatch frames as fast as buffers arrive
 * and overrun the descriptor ring. Holding at rkvdec_link_depth tasks in
 * flight is what lets the completion path reap and try_schedule the next.
 *
 * Single-shot (link_mode=0 or no link_table) returns 1 — m2m's own
 * buffer-readiness check is the only gate, exactly as before.
 */
static int rkvdec_job_ready(void *priv)
{
	struct rkvdec_ctx *ctx = priv;

	/*
	 * vdpu383-submit Phase 1b: batch-when-idle. In kthread mode
	 * (rkvdec_submit_mode=2) only let m2m dispatch device_run when HW is
	 * fully idle (inflight==0). device_run then batches every currently-
	 * ready frame into ONE bootstrap kick (frame_num=N), so HW never gets a
	 * mid-decode ADD_MODE append — which is what disturbed in-flight frames
	 * into partials and forced the watchdog reset+resend recovery that
	 * dominated the depth>1 throughput gap (SESSION_2026-06-07_kthread_phase1a:
	 * 14 resets / 600 frames, ~half the 89→160 fps gap). No append => no
	 * partials => no resends. (depth>2 batched still hits the IP ring-walk
	 * limit, a separate issue.)
	 */
	if (rkvdec_submit_mode == 2 && rkvdec_link_mode && ctx->link_table)
		return rkvdec_inflight_depth(ctx) == 0 ? 1 : 0;

	if (rkvdec_link_mode && ctx->link_table &&
	    rkvdec_inflight_depth(ctx) >= (u32)rkvdec_link_depth)
		return 0;

	return 1;
}

static const struct v4l2_m2m_ops rkvdec_m2m_ops = {
	.device_run = rkvdec_device_run,
	.job_abort  = rkvdec_job_abort,
	.job_ready  = rkvdec_job_ready,
};

static int rkvdec_queue_init(void *priv,
			     struct vb2_queue *src_vq,
			     struct vb2_queue *dst_vq)
{
	struct rkvdec_ctx *ctx = priv;
	struct rkvdec_dev *rkvdec = ctx->dev;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &rkvdec_queue_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;

	/*
	 * Driver does mostly sequential access, so sacrifice TLB efficiency
	 * for faster allocation. Also, no CPU access on the source queue,
	 * so no kernel mapping needed.
	 */
	src_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES |
			    DMA_ATTR_NO_KERNEL_MAPPING;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &rkvdec->vdev_lock;
	src_vq->dev = rkvdec->v4l2_dev.dev;
	src_vq->supports_requests = true;
	src_vq->requires_requests = true;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->bidirectional = true;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES |
			    DMA_ATTR_NO_KERNEL_MAPPING;
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &rkvdec_queue_ops;
	dst_vq->buf_struct_size = sizeof(struct rkvdec_decoded_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &rkvdec->vdev_lock;
	dst_vq->dev = rkvdec->v4l2_dev.dev;

	return vb2_queue_init(dst_vq);
}

static int rkvdec_add_ctrls(struct rkvdec_ctx *ctx,
			    const struct rkvdec_ctrls *ctrls)
{
	unsigned int i;

	for (i = 0; i < ctrls->num_ctrls; i++) {
		const struct v4l2_ctrl_config *cfg = &ctrls->ctrls[i].cfg;

		v4l2_ctrl_new_custom(&ctx->ctrl_hdl, cfg, ctx);
		if (ctx->ctrl_hdl.error)
			return ctx->ctrl_hdl.error;
	}

	return 0;
}

static int rkvdec_init_ctrls(struct rkvdec_ctx *ctx)
{
	const struct rkvdec_variant *variant = ctx->dev->variant;
	unsigned int i, nctrls = 0;
	int ret;

	for (i = 0; i < variant->num_coded_fmts; i++)
		nctrls += variant->coded_fmts[i].ctrls->num_ctrls;

	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, nctrls);

	for (i = 0; i < variant->num_coded_fmts; i++) {
		ret = rkvdec_add_ctrls(ctx, variant->coded_fmts[i].ctrls);
		if (ret)
			goto err_free_handler;
	}

	ret = v4l2_ctrl_handler_setup(&ctx->ctrl_hdl);
	if (ret)
		goto err_free_handler;

	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;
	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	return ret;
}

static int rkvdec_open(struct file *filp)
{
	struct rkvdec_dev *rkvdec = video_drvdata(filp);
	struct rkvdec_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = rkvdec;
	rkvdec->accum_open++;
	rkvdec_reset_coded_fmt(ctx);
	rkvdec_reset_decoded_fmt(ctx);
	v4l2_fh_init(&ctx->fh, video_devdata(filp));

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(rkvdec->m2m_dev, ctx,
					    rkvdec_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_free_ctx;
	}

	ret = rkvdec_init_ctrls(ctx);
	if (ret)
		goto err_cleanup_m2m_ctx;

	v4l2_fh_add(&ctx->fh, filp);

	return 0;

err_cleanup_m2m_ctx:
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

err_free_ctx:
	kfree(ctx);
	return ret;
}

static int rkvdec_release(struct file *filp)
{
	struct rkvdec_ctx *ctx = file_to_rkvdec_ctx(filp);

	ctx->dev->accum_release++;
	v4l2_fh_del(&ctx->fh, filp);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations rkvdec_fops = {
	.owner = THIS_MODULE,
	.open = rkvdec_open,
	.release = rkvdec_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int rkvdec_v4l2_init(struct rkvdec_dev *rkvdec)
{
	int ret;

	ret = v4l2_device_register(rkvdec->dev, &rkvdec->v4l2_dev);
	if (ret) {
		dev_err(rkvdec->dev, "Failed to register V4L2 device\n");
		return ret;
	}

	rkvdec->m2m_dev = v4l2_m2m_init(&rkvdec_m2m_ops);
	if (IS_ERR(rkvdec->m2m_dev)) {
		v4l2_err(&rkvdec->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(rkvdec->m2m_dev);
		goto err_unregister_v4l2;
	}

	rkvdec->mdev.dev = rkvdec->dev;
	strscpy(rkvdec->mdev.model, "rkvdec", sizeof(rkvdec->mdev.model));
	strscpy(rkvdec->mdev.bus_info, "platform:rkvdec",
		sizeof(rkvdec->mdev.bus_info));
	media_device_init(&rkvdec->mdev);
	rkvdec->mdev.ops = &rkvdec_media_ops;
	rkvdec->v4l2_dev.mdev = &rkvdec->mdev;

	rkvdec->vdev.lock = &rkvdec->vdev_lock;
	rkvdec->vdev.v4l2_dev = &rkvdec->v4l2_dev;
	rkvdec->vdev.fops = &rkvdec_fops;
	rkvdec->vdev.release = video_device_release_empty;
	rkvdec->vdev.vfl_dir = VFL_DIR_M2M;
	rkvdec->vdev.device_caps = V4L2_CAP_STREAMING |
				   V4L2_CAP_VIDEO_M2M_MPLANE;
	rkvdec->vdev.ioctl_ops = &rkvdec_ioctl_ops;
	video_set_drvdata(&rkvdec->vdev, rkvdec);
	strscpy(rkvdec->vdev.name, "rkvdec", sizeof(rkvdec->vdev.name));

	ret = video_register_device(&rkvdec->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&rkvdec->v4l2_dev, "Failed to register video device\n");
		goto err_cleanup_mc;
	}

	ret = v4l2_m2m_register_media_controller(rkvdec->m2m_dev, &rkvdec->vdev,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		v4l2_err(&rkvdec->v4l2_dev,
			 "Failed to initialize V4L2 M2M media controller\n");
		goto err_unregister_vdev;
	}

	ret = media_device_register(&rkvdec->mdev);
	if (ret) {
		v4l2_err(&rkvdec->v4l2_dev, "Failed to register media device\n");
		goto err_unregister_mc;
	}

	return 0;

err_unregister_mc:
	v4l2_m2m_unregister_media_controller(rkvdec->m2m_dev);

err_unregister_vdev:
	video_unregister_device(&rkvdec->vdev);

err_cleanup_mc:
	media_device_cleanup(&rkvdec->mdev);
	v4l2_m2m_release(rkvdec->m2m_dev);

err_unregister_v4l2:
	v4l2_device_unregister(&rkvdec->v4l2_dev);
	return ret;
}

static void rkvdec_v4l2_cleanup(struct rkvdec_dev *rkvdec)
{
	media_device_unregister(&rkvdec->mdev);
	v4l2_m2m_unregister_media_controller(rkvdec->m2m_dev);
	video_unregister_device(&rkvdec->vdev);
	media_device_cleanup(&rkvdec->mdev);
	v4l2_m2m_release(rkvdec->m2m_dev);
	v4l2_device_unregister(&rkvdec->v4l2_dev);
}

static void rkvdec_iommu_restore(struct rkvdec_dev *rkvdec)
{
	if (rkvdec->empty_domain) {
		/*
		 * To rewrite mapping into the attached IOMMU core, attach a new empty domain that
		 * will program an empty table, then detach it to restore the default domain and
		 * all cached mappings.
		 * This is safely done in this interrupt handler to make sure no memory get mapped
		 * through the IOMMU while the empty domain is attached.
		 */
		iommu_attach_device(rkvdec->empty_domain, rkvdec->dev);
		iommu_detach_device(rkvdec->empty_domain, rkvdec->dev);
	}
	/*
	 * Re-attaching the domain can leave stale TLB state — force the next
	 * kick to flush (flush-only-after-restore throughput fix). This is the
	 * one case where the per-frame flush was actually load-bearing.
	 */
	rkvdec->iommu_needs_flush = true;
}

static irqreturn_t rk3399_irq_handler(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	enum vb2_buffer_state state;
	u32 status;

	status = readl(rkvdec->regs + RKVDEC_REG_INTERRUPT);
	writel(0, rkvdec->regs + RKVDEC_REG_INTERRUPT);

	if (status & RKVDEC_RDY_STA) {
		state = VB2_BUF_STATE_DONE;
	} else {
		pr_warn_ratelimited("rkvdec: hw ERROR sta=0x%08x\n", status);
		state = VB2_BUF_STATE_ERROR;
		if (status & RKVDEC_SOFTRESET_RDY)
			rkvdec_iommu_restore(rkvdec);
	}

	if (cancel_delayed_work(&rkvdec->watchdog_work))
		rkvdec_job_finish(ctx, state);

	return IRQ_HANDLED;
}

static irqreturn_t vdpu381_irq_handler(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	enum vb2_buffer_state state;
	u32 status;

	status = readl(rkvdec->regs + VDPU381_REG_STA_INT);
	writel(0, rkvdec->regs + VDPU381_REG_STA_INT);

	if (status & VDPU381_STA_INT_DEC_RDY_STA) {
		state = VB2_BUF_STATE_DONE;
	} else {
		pr_warn_ratelimited("rkvdec: hw ERROR sta=0x%08x\n", status);
		state = VB2_BUF_STATE_ERROR;
		if (status & (VDPU381_STA_INT_SOFTRESET_RDY |
			      VDPU381_STA_INT_TIMEOUT |
			      VDPU381_STA_INT_ERROR))
			rkvdec_iommu_restore(rkvdec);
	}

	if (cancel_delayed_work(&rkvdec->watchdog_work))
		rkvdec_job_finish(ctx, state);

	return IRQ_HANDLED;
}


static irqreturn_t vdpu383_irq_handler(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	enum vb2_buffer_state state;
	u32 status;
	/*
	 * irq_split: deferred-completion bookkeeping. The error branch records
	 * whether an iommu_restore is needed instead of doing it inline (it
	 * sleeps); the completion below queues the work item rather than
	 * calling job_finish (also sleeps via the done() op). Unused when
	 * irq_split=0 — the legacy inline calls run exactly as before.
	 */
	bool need_iommu_restore = false;

	if (rkvdec_link_mode && ctx->link_table) {
		/* Link mode: BSP-faithful task completion semantics.
		 * mpp_rkvdec2_link.c:1119-1120:
		 *   irq_status = tb_reg[info->tb_reg_int];
		 *   task_done = irq_status || timeout_flag || abort_flag;
		 * i.e. ANY non-zero irq_status counts as task done. BSP
		 * only soft-resets HW on SW-timeout / abort / IOMMU fault
		 * (line 1152), NOT on HW error bits in irq_status itself.
		 *
		 * Our v0.7-v0.13 was over-reset: any non-BIT(0) triggered
		 * soft-reset which destroyed HW state needed for the next
		 * frame, contributing to the per-task settle gap. */
		u32 task_status = rkvdec_link_read_task_status(
			ctx->link_table, 0);
		bool task_done = (task_status != 0xffffffffu) &&
				 (task_status != 0);
		bool clean = task_done && (task_status & BIT(0));

		writel(FIELD_PREP_WM16(VDPU383_INT_EN_IRQ |
				       VDPU383_INT_EN_LINE_IRQ, 0),
		       rkvdec->link + VDPU383_LINK_INT_EN);

		/* HW timing fix: a small delay between IRQ ack and the
		 * codec irq handler returning lets HW state settle before
		 * the next vp9_run's enqueue. Empirically discovered via
		 * pr_info side effect: q6 frame 0 went from core_TO to
		 * DONE when a printk landed here.
		 *
		 * Fluster intra-only subset pass rate vs delay:
		 *   0 us  -> 1/9
		 *   50 us -> 4/9 (matches pr_info baseline)
		 *
		 * Sintel/BBB (INTER content) DONE/ERROR/timeout per 30
		 * buffers, fresh insmod per file:
		 *   no delay        -> 1/9 Fluster intra
		 *   udelay 50       -> 4/9 (variable 1-4/5 across runs)
		 *   mb()+readl flush -> 3/5,1/5,1/5 (same variability)
		 *   delay moved to vp9_run pre-enqueue -> TBD this run
		 */

		/* R39 (2026-06-01): unrate-limited per-IRQ log — every IRQ
		 * fires here so we can see the full classification stream
		 * for bistability debugging. */
		pr_info("rkvdec-vp9: link %s task_status=0x%08x\n",
			clean ? "DONE-clean" :
			task_done ? "DONE-err" : "NO-WB",
			task_status);

		/* Step A telemetry: classify which completion path this
		 * IRQ took. clean = HW signalled DEC_RDY (BIT(0)) in the
		 * descriptor write-back. err = HW wrote a non-DEC_RDY
		 * value (e.g. error bits). nowb = no write-back yet
		 * (HW IRQ fired before descriptor was written, or stale
		 * sentinel). The watchdog handler increments separate
		 * silent_completion / real_timeout counters. */
		if (clean)
			rkvdec->telem_irq_done_clean++;
		else if (task_done)
			rkvdec->telem_irq_done_err++;
		else
			rkvdec->telem_irq_nowb++;

		/* BSP only soft-resets on SW-timeout / abort / IOMMU
		 * fault, not on HW error bits. Suppress the soft-reset
		 * for non-clean-DONE cases unless we have evidence of
		 * one of those specific conditions. */
		if (false) {
			u32 ip_en = readl(rkvdec->link +
					  VDPU383_LINK_IP_ENABLE);
			int poll;

			writel(ip_en | BIT(15),
			       rkvdec->link + VDPU383_LINK_IP_ENABLE);
			writel(BIT(0), rkvdec->link + 0x44);
			for (poll = 0; poll < 50; poll++) {
				if (readl(rkvdec->link +
					  VDPU383_LINK_STA_INT) & BIT(11))
					break;
				udelay(10);
			}
			writel(FIELD_PREP_WM16(BIT(11), 0),
			       rkvdec->link + VDPU383_LINK_STA_INT);
			writel(FIELD_PREP_WM16(0xffff, 0),
			       rkvdec->link + VDPU383_LINK_INT_EN);
			writel(FIELD_PREP_WM16(VDPU383_STA_INT_ALL, 0),
			       rkvdec->link + VDPU383_LINK_STA_INT);
			writel(ip_en,
			       rkvdec->link + VDPU383_LINK_IP_ENABLE);
			ctx->link_table->expected_dec_num = 0;
		}

		/*
		 * Phase 3 v0.3 step 2.6: drain `last_irq_completed`
		 * inflight entries inline. With batched fill (depth>1) HW
		 * can complete several tasks before raising one IRQ, so
		 * the IRQ handler can't assume completed==1.
		 *
		 * cancel_delayed_work returning true means we own the
		 * watchdog cycle (it was still pending, hasn't run). If
		 * inflight is fully drained we own the pm_put and
		 * job_finish; if more tasks are still in flight we
		 * reschedule the watchdog (so a later silent_completion
		 * isn't left without timeout coverage) and keep pm
		 * elevated until the last drain.
		 *
		 * If cancel returns false the watchdog has run (or is
		 * running). Its rkvdec_job_finish_no_pm path drains
		 * what's left of the ring and calls pm_put + job_finish
		 * itself, so we do nothing here.
		 */
		if (cancel_delayed_work(&rkvdec->watchdog_work)) {
			enum vb2_buffer_state state = task_done
				? VB2_BUF_STATE_DONE
				: VB2_BUF_STATE_ERROR;
			u32 completed = ctx->last_irq_completed;
			u32 i;

			ctx->last_irq_completed = 0;
			for (i = 0; i < completed; i++) {
				struct rkvdec_link_inflight e =
					rkvdec_inflight_pop(ctx);

				if (!e.src || !e.dst) {
					pr_warn_ratelimited(
						"rkvdec: irq drain %u of %u: inflight empty\n",
						i, completed);
					break;
				}
				if (ctx->coded_fmt_desc->ops->done)
					ctx->coded_fmt_desc->ops->done(
						ctx, e.src, e.dst, state);
				v4l2_m2m_buf_done(e.src, state);
				v4l2_m2m_buf_done(e.dst, state);
			}

			if (rkvdec_inflight_depth(ctx) == 0) {
				pm_runtime_put_autosuspend(rkvdec->dev);
				v4l2_m2m_job_finish(rkvdec->m2m_dev,
						    ctx->fh.m2m_ctx);
			} else {
				/* Batch not fully drained yet. Re-arm
				 * watchdog at the fast-poll interval so
				 * a silent-completion on a later task is
				 * detected quickly. Reset poll counter
				 * for the new task. */
				ctx->watchdog_poll_count = 0;
				rkvdec_schedule_watchdog_poll(rkvdec);
			}
		}
		return IRQ_HANDLED;
	}

	status = readl(rkvdec->link + VDPU383_LINK_STA_INT);
	writel(FIELD_PREP_WM16(VDPU383_STA_INT_ALL, 0),
	       rkvdec->link + VDPU383_LINK_STA_INT);
	/* On vdpu383, the interrupts must be disabled */
	writel(FIELD_PREP_WM16(VDPU383_INT_EN_IRQ | VDPU383_INT_EN_LINE_IRQ, 0),
	       rkvdec->link + VDPU383_LINK_INT_EN);

	/*
	 * R39/R42 (2026-06-01) — LOAD-BEARING pr_info. The per-IRQ log
	 * was originally diagnostic, but removing it regressed yt_720p60
	 * perfect rate from ~25% to 0%. The pr_info side-effect adds
	 * variable CPU work (~50 us + printk lock cost) between IRQ ack
	 * and handler return that HW state needs to settle before the
	 * next vp9_run's enqueue.
	 *
	 * Replacing with udelay() at the same site caused soft-lockup
	 * crashes when sweeping > 100 us in the IRQ handler.
	 *
	 * KEEP this pr_info unless replaced with an out-of-handler
	 * settle mechanism (e.g. mdelay in vp9_run pre-kick).
	 */
	pr_info("rkvdec-vp9: ss IRQ sta=0x%08x\n", status);

	if (status & VDPU383_STA_INT_DEC_RDY_STA) {
		/* Even when DEC_RDY_STA is set, reg15 can carry secondary
		 * error bits (bus_error / colmv_ref_err / wlast_miss /
		 * strm_err) that indicate the decoded buffer is partially
		 * corrupted. Surface them as a warning so userspace can
		 * decide whether to drop the frame. */
		u32 irq_sta = readl(rkvdec->regs +
				    VDPU383_OFFSET_COMMON_REGS + 7 * 4);
		if (irq_sta)
			pr_warn_ratelimited("rkvdec: DEC_RDY+irq_sta=0x%08x (%s%s%s%s%s%s%s%s%s)\n",
				irq_sta,
				(irq_sta & BIT(1)) ? "strm_err " : "",
				(irq_sta & BIT(2)) ? "core_TO " : "",
				(irq_sta & BIT(3)) ? "ip_TO " : "",
				(irq_sta & BIT(4)) ? "bus_err " : "",
				(irq_sta & BIT(5)) ? "buf_empty " : "",
				(irq_sta & BIT(6)) ? "colmv_ref_err " : "",
				(irq_sta & BIT(7)) ? "err_spread " : "",
				(irq_sta & BIT(8)) ? "create_TO " : "",
				(irq_sta & BIT(9)) ? "wlast_miss " : "");
		state = VB2_BUF_STATE_DONE;
		/* Step A telemetry control test: count single-shot
		 * IRQ-DONE-clean events so we can compare against link
		 * mode's irq_clean. If single-shot's irq_clean roughly
		 * equals total task count, IRQ-driven mode works fine
		 * for single-shot; the silent-completion is then
		 * link-mode-specific. */
		rkvdec->telem_irq_done_clean++;
		/*
		 * vp9_time diagnostic: split the kick->DONE interval into
		 * kick->hardirq (pure HW decode) and hardirq->threaded (IRQ +
		 * scheduler wake latency). This is the decisive
		 * silicon-vs-driver-overhead measurement: if HW decode is small
		 * and IRQ latency is large, the single-shot throughput gap vs MPP
		 * is removable driver overhead, not silicon.
		 */
		if (vp9_time && rkvdec->vp9_kick_kt) {
			ktime_t now = ktime_get();
			u64 hw_ns = rkvdec->vp9_hardirq_kt ?
				ktime_to_ns(ktime_sub(rkvdec->vp9_hardirq_kt,
						      rkvdec->vp9_kick_kt)) :
				ktime_to_ns(ktime_sub(now, rkvdec->vp9_kick_kt));
			u64 lat_ns = rkvdec->vp9_hardirq_kt ?
				ktime_to_ns(ktime_sub(now,
						      rkvdec->vp9_hardirq_kt)) : 0;

			rkvdec->vp9_dec_ns_sum += hw_ns;
			rkvdec->vp9_irq_ns_sum += lat_ns;
			if (++rkvdec->vp9_dec_ns_cnt >= 100) {
				u32 n = rkvdec->vp9_dec_ns_cnt;
				/*
				 * 2026-06-07 memory-bound probe: read the AXI perf
				 * counters (reg322-325 @ 0x508-0x514) for THIS decode.
				 * rd_total_bytes = bytes read over AXI (ref + stream);
				 * avg/max_lat = read latency in counter ticks. High
				 * bytes + high latency => memory-bandwidth-bound; compare
				 * rd_total_bytes vs MPP (FBC would roughly halve it).
				 * Enabled only when vp9_time set (config_registers).
				 */
				u32 rd_maxlat = readl(rkvdec->regs + 0x508);
				u32 rd_samp   = readl(rkvdec->regs + 0x50c);
				u32 rd_accsum = readl(rkvdec->regs + 0x510);
				u32 rd_bytes  = readl(rkvdec->regs + 0x514);

				pr_info("rkvdec: HWdecode=%llu us IRQlat=%llu us total=%llu us/frame over %u\n",
					rkvdec->vp9_dec_ns_sum / n / 1000,
					rkvdec->vp9_irq_ns_sum / n / 1000,
					(rkvdec->vp9_dec_ns_sum +
					 rkvdec->vp9_irq_ns_sum) / n / 1000, n);
				pr_info("rkvdec: perf rd_total_bytes=%u avg_lat=%u max_lat=%u samp=%u (last frame)\n",
					rd_bytes, rd_samp ? rd_accsum / rd_samp : 0,
					rd_maxlat, rd_samp);
				rkvdec->vp9_dec_ns_sum = 0;
				rkvdec->vp9_irq_ns_sum = 0;
				rkvdec->vp9_dec_ns_cnt = 0;
			}
			rkvdec->vp9_kick_kt = 0;
			rkvdec->vp9_hardirq_kt = 0;
		}
	} else if (status & VDPU383_INT_EN_LINE_IRQ) {
		/* Line interrupt fired before frame-done: decode still running.
		 * Re-enable the frame-done IRQ only and let it complete.
		 *
		 * R39 (2026-06-01) bug fix: the prior condition
		 * `!(status & ~BIT(1))` ALSO matched status == 0
		 * (since `!(0 & ~BIT(1))` is `!(0)` = true). When HW
		 * asserted a spurious IRQ with status=0 (stale-IRQ /
		 * electrical) we'd re-enable INT_EN and immediately re-fire,
		 * creating a tight ~5µs IRQ storm until the 16 ms m2m
		 * watchdog timed out. That's the per-frame "thrash" we
		 * observed in `silent_no_wb` cascades.
		 *
		 * Capturing the dmesg of a good vs bad 30-frame trial
		 * proved it directly: good trial = sta=0x01 IRQs spaced
		 * 1-4 ms; bad trial = sta=0x00 IRQs spaced 5 µs in a
		 * ~1 ms burst, then m2m timeout. Same content, same
		 * registers, same DMA — only difference was whether HW
		 * happened to set BIT(0) properly or fired spurious IRQs
		 * with status=0.
		 *
		 * Requiring BIT(1) actually set narrows this branch to its
		 * intended semantic ("real line IRQ"); spurious status=0
		 * IRQs now fall to the error branch below and trigger
		 * proper soft-reset instead of the tight loop.
		 */
		writel(FIELD_PREP_WM16(VDPU383_INT_EN_IRQ, VDPU383_INT_EN_IRQ),
		       rkvdec->link + VDPU383_LINK_INT_EN);
		return IRQ_HANDLED;
	} else if (status == 0) {
		/* R42 (2026-06-01) — status=0 is a "decode in progress"
		 * signal, not a storm. Re-enable INT_EN_IRQ so the real
		 * DEC_RDY can fire when decode completes.
		 *
		 * R39 fix made this ack-only to break the per-spurious-IRQ
		 * tight loop, but the cost was killing legitimate decode
		 * IRQ delivery on the alt-ref-thrash path: HW asserts a
		 * status=0 IRQ first (a sort of "in progress" signal),
		 * then asserts DEC_RDY when actually done. If we ack-only
		 * on the first, the second never delivers and the frame
		 * times out.
		 *
		 * To prevent the storm without losing legitimate IRQs:
		 * brief udelay before re-enable. This gives HW time to
		 * advance state past whatever was causing the transient
		 * status=0 assertion. Empirically tunable; start with
		 * 100 µs which is < normal per-frame decode time but
		 * long enough to skip a single transient.
		 */
		udelay(100);
		writel(FIELD_PREP_WM16(VDPU383_INT_EN_IRQ, VDPU383_INT_EN_IRQ),
		       rkvdec->link + VDPU383_LINK_INT_EN);
		return IRQ_HANDLED;
	} else if (status == BIT(2) &&
		   readl(rkvdec->regs + VDPU383_OFFSET_COMMON_REGS + 7 * 4) == 0) {
		/* LINK_STA_INT bit 2 alone with codec reg15 clean (no error
		 * bits set) means the IP block completed via an alternate
		 * completion path, not the usual DEC_RDY_STA (bit 0) signal.
		 * Empirically observed on AV1 INTER decodes - the decoded
		 * output is valid.  Treat as DONE rather than erroring out. */
		state = VB2_BUF_STATE_DONE;
	} else {
		/* Read the fine-grained per-subsystem status (reg015_irq_sta)
		 * to know which path tripped. Decode bits:
		 *   bit 0: frame_rdy           bit 5: buffer_empty
		 *   bit 1: strm_error          bit 6: colmv_ref_error
		 *   bit 2: core_timeout        bit 7: error_spread
		 *   bit 3: ip_timeout          bit 8: create_core_timeout
		 *   bit 4: bus_error           bit 9: wlast_miss_match
		 * reg015 lives at byte offset 60 inside the codec regs window
		 * (VDPU383_OFFSET_COMMON_REGS=32, then 7*4 for regs 8..14). */
		u32 irq_sta = readl(rkvdec->regs +
				    VDPU383_OFFSET_COMMON_REGS + 7 * 4);
		pr_warn_ratelimited("rkvdec: hw ERROR sta=0x%08x irq_sta=0x%08x (bits: %s%s%s%s%s%s%s%s%s)\n",
			status, irq_sta,
			(irq_sta & BIT(1)) ? "strm_err " : "",
			(irq_sta & BIT(2)) ? "core_TO " : "",
			(irq_sta & BIT(3)) ? "ip_TO " : "",
			(irq_sta & BIT(4)) ? "bus_err " : "",
			(irq_sta & BIT(5)) ? "buf_empty " : "",
			(irq_sta & BIT(6)) ? "colmv_ref_err " : "",
			(irq_sta & BIT(7)) ? "err_spread " : "",
			(irq_sta & BIT(8)) ? "create_TO " : "",
			(irq_sta & BIT(9)) ? "wlast_miss " : "");
		state = VB2_BUF_STATE_ERROR;
		/* Only restore IOMMU for genuine bus/IOMMU faults (bus_error=bit4,
		 * colmv_ref_error=bit6, wlast_miss_match=bit9). core_timeout (bit2)
		 * leaves the IOMMU clean; the unnecessary restore adds ~4.8s overhead
		 * on every Bug A tiny-frame timeout. */
		if (irq_sta & (BIT(4) | BIT(6) | BIT(9))) {
			if (irq_split)
				need_iommu_restore = true; /* deferred to work */
			else
				rkvdec_iommu_restore(rkvdec); /* legacy inline */
		}
	}

	if (cancel_delayed_work(&rkvdec->watchdog_work)) {
		if (irq_split) {
			/*
			 * Hardirq context (top-half called us): defer the
			 * sleeping completion to the high-priority work item.
			 * Safe at depth=1 — one completion in flight, the next
			 * IRQ cannot fire until this job_finish dispatches the
			 * next device_run.
			 */
			rkvdec->irq_done_ctx = ctx;
			rkvdec->irq_done_state = state;
			rkvdec->irq_done_iommu_restore = need_iommu_restore;
			queue_work(system_highpri_wq, &rkvdec->irq_done_work);
		} else {
			rkvdec_job_finish(ctx, state);
		}
	}

	return IRQ_HANDLED;
}

/*
 * Bottom-half for the irq_split throughput experiment. Runs in process
 * context (system_highpri_wq) so the sleeping completion calls are legal:
 * iommu_restore (iommu_attach/detach mutexes) and rkvdec_job_finish (the
 * codec done() op takes the ctrl-handler mutex via v4l2_ctrl_find).
 */
static void rkvdec_irq_done_work_fn(struct work_struct *work)
{
	struct rkvdec_dev *rkvdec =
		container_of(work, struct rkvdec_dev, irq_done_work);
	struct rkvdec_ctx *ctx = rkvdec->irq_done_ctx;

	if (!ctx)
		return;
	if (rkvdec->irq_done_iommu_restore) {
		rkvdec->irq_done_iommu_restore = false;
		rkvdec_iommu_restore(rkvdec);
	}
	rkvdec_job_finish(ctx, rkvdec->irq_done_state);
}

/*
 * Hardirq top-half for the irq_split throughput experiment. Default off:
 * returns IRQ_WAKE_THREAD so the legacy pure-threaded handler runs unchanged.
 * On: classifies + acks inline for the single-shot vdpu383 path only and
 * returns IRQ_HANDLED, having deferred the sleeping completion to the work
 * item. Link mode and other variants are not split-safe -> thread.
 */
static irqreturn_t rkvdec_irq_top(int irq, void *priv)
{
	struct rkvdec_dev *rkvdec = priv;
	struct rkvdec_ctx *ctx;

	/*
	 * 2026-06-08 Variant B: IRQ-driven warmup reap. The warmup runs with NO
	 * decode job/ctx (probe / runtime_resume), so it must be caught HERE in the
	 * top-level handler, before any ctx lookup. Disable the link IRQ (WM16 clear
	 * of INT_EN bit0) so it can't re-fire, leave the 0x4c status for
	 * warmup_run_irq to read, and wake it. Scoped by warmup_irq_inflight so it
	 * never touches the decode reap. Hardirq-safe (MMIO + complete()).
	 */
	if (rkvdec->warmup_irq_inflight) {
		writel(0xffff0000u, rkvdec->link + 0x048); /* WM16: mask=all, val=0 */
		rkvdec->warmup_irq_inflight = false;
		complete(&rkvdec->warmup_irq_done);
		return IRQ_HANDLED;
	}

	/*
	 * vp9_time diagnostic: stamp the hardirq arrival so the threaded DONE
	 * handler can split kick->hardirq (pure HW decode time) from
	 * hardirq->threaded (IRQ delivery + scheduler wake latency). Overwrite
	 * on every IRQ so the final (DONE) hardirq wins over any earlier line
	 * IRQ. Gated on vp9_kick_kt (set only between a kick and its DONE).
	 */
	if (vp9_time && rkvdec->vp9_kick_kt)
		rkvdec->vp9_hardirq_kt = ktime_get();

	/*
	 * vdpu383-submit Phase 1a (rkvdec_submit_mode=2): kthread-worker link
	 * completion. Ack the link IRQ here in the hardirq top-half — the SAME
	 * WM16 clear (0xffff0000 to irq_base 0x48 + status_base 0x4c) the
	 * threaded link handler does, MMIO-only and hardirq-safe — then wake the
	 * resident worker to run the sleeping per-slot reap. Returning
	 * IRQ_HANDLED skips the threaded handler so its inline reap doesn't also
	 * run. Falls through (untouched) if the worker is absent or there's no
	 * link ctx in flight.
	 */
	if (rkvdec_submit_mode == 2 && rkvdec_link_mode && rkvdec->link_worker) {
		struct rkvdec_ctx *lctx = rkvdec->link_ctx;

		if (lctx && lctx->link_table) {
			writel(0xffff0000u, rkvdec->link + 0x48);
			writel(0xffff0000u, rkvdec->link + 0x4c);
			kthread_queue_work(rkvdec->link_worker,
					   &rkvdec->link_reap_work);
			return IRQ_HANDLED;
		}
	}

	if (!irq_split)
		return IRQ_WAKE_THREAD;
	if (rkvdec_link_mode)
		return IRQ_WAKE_THREAD;
	if (rkvdec->variant->ops->irq_handler != vdpu383_irq_handler)
		return IRQ_WAKE_THREAD;

	ctx = v4l2_m2m_get_curr_priv(rkvdec->m2m_dev);
	if (!ctx)
		return IRQ_NONE;

	/*
	 * vdpu383_irq_handler does hardirq-safe MMIO (status read, ack,
	 * in-progress re-enable with a bounded udelay) and, in irq_split mode,
	 * defers iommu_restore + job_finish to rkvdec->irq_done_work.
	 */
	return rkvdec->variant->ops->irq_handler(ctx);
}

static irqreturn_t rkvdec_irq_handler(int irq, void *priv)
{
	struct rkvdec_dev *rkvdec = priv;
	struct rkvdec_ctx *ctx = v4l2_m2m_get_curr_priv(rkvdec->m2m_dev);
	const struct rkvdec_variant *variant = rkvdec->variant;

	/* Link mode job_finishes at submit, so get_curr_priv() is NULL by
	 * the time the IRQ fires; use the stashed completion-owner ctx so we
	 * can still ack. (LINK_MODE_PORT_DESIGN_2026-06-01 §3) */
	if (!ctx && rkvdec_link_mode)
		ctx = rkvdec->link_ctx;
	if (!ctx)
		return IRQ_NONE;

	/*
	 * Link mode — BSP-faithful completion (2026-06-01 BSP .104 capture):
	 * HW fires the link IRQ and writes each task's status into that
	 * descriptor's tb_reg_int slot. Ack the status + IRQ-enable so the
	 * line de-asserts, then reap every task whose per-slot writeback is
	 * set (rkvdec_link_reap_writeback, mirrors rkvdec2_link_try_dequeue).
	 * Runs in threaded IRQ context, so done()/buf_done may sleep. The
	 * old "ack-only, watchdog owns completion" path was wrong: it keyed
	 * on dec_num (never advances) / slot-0 writeback (ring rotates), and
	 * discarded the very IRQ that signals completion (the "290 spurious").
	 */
	if (rkvdec_link_mode && ctx->link_table) {
		/*
		 * Ack exactly like BSP rkvdec_vdpu383_link_irq: write
		 * 0xffff0000 (WM16: mask all 16, value 0) to BOTH irq_base
		 * (0x48) and status_base (0x4c) to clear ALL pending status
		 * bits. The earlier partial clear (only IRQ|LINE on 0x48,
		 * low-10 on 0x4c) left other status bits set, which stopped HW
		 * arming the next writeback/IRQ and made completion bistable
		 * per session (clean OR whole-session timeout). Then reap.
		 */
		writel(0xffff0000u, rkvdec->link + 0x48);
		writel(0xffff0000u, rkvdec->link + 0x4c);
		rkvdec_link_reap_writeback(ctx);
		return IRQ_HANDLED;
	}

	return variant->ops->irq_handler(ctx);
}

/*
 * Flip one or more matrices along their main diagonal and flatten them
 * before writing it to the memory.
 * Convert:
 * ABCD         AEIM
 * EFGH     =>  BFJN     =>     AEIMBFJNCGKODHLP
 * IJKL         CGKO
 * MNOP         DHLP
 */
static void transpose_and_flatten_matrices(u8 *output, const u8 *input,
					   int matrices, int row_length)
{
	int i, j, row, x_offset, matrix_offset, rot_index, y_offset, matrix_size, new_value;

	matrix_size = row_length * row_length;
	for (i = 0; i < matrices; i++) {
		row = 0;
		x_offset = 0;
		matrix_offset = i * matrix_size;
		for (j = 0; j < matrix_size; j++) {
			y_offset = j - (row * row_length);
			rot_index = y_offset * row_length + x_offset;
			new_value = *(input + i * matrix_size + j);
			output[matrix_offset + rot_index] = new_value;
			if ((j + 1) % row_length == 0) {
				row += 1;
				x_offset += 1;
			}
		}
	}
}

/*
 * VDPU383 needs a specific order:
 * The 8x8 flatten matrix is based on 4x4 blocks.
 * Each 4x4 block is written separately in order.
 *
 * Base data    =>  Transposed    VDPU383 transposed
 *
 * ABCDEFGH         AIQYaiqy      AIQYBJRZ
 * IJKLMNOP         BJRZbjrz      CKS0DLT1
 * QRSTUVWX         CKS0cks6      aiqybjrz
 * YZ012345     =>  DLT1dlt7      cks6dlt7
 * abcdefgh         EMU2emu8      EMU2FNV3
 * ijklmnop         FNV3fnv9      GOW4HPX5
 * qrstuvwx         GOW4gow#      emu8fnv9
 * yz6789#$         HPX5hpx$      gow#hpx$
 *
 * As the function reads block of 4x4 it can be used for both 4x4 and 8x8 matrices.
 *
 */
static void vdpu383_flatten_matrices(u8 *output, const u8 *input, int matrices, int row_length)
{
	u8 block;
	int i, j, matrix_offset, matrix_size, new_value, input_idx, line_offset, block_offset;

	matrix_size = row_length * row_length;
	for (i = 0; i < matrices; i++) {
		matrix_offset = i * matrix_size;
		for (j = 0; j < matrix_size; j++) {
			block = j / 16;
			line_offset = (j % 16) / 4;
			block_offset = (block & 1) * 32 + (block & 2) * 2;
			input_idx = ((j % 4) * row_length) + line_offset + block_offset;

			new_value = *(input + i * matrix_size + input_idx);

			output[matrix_offset + j] = new_value;
		}
	}
}

static void rkvdec_watchdog_func(struct work_struct *work)
{
	struct rkvdec_dev *rkvdec;
	struct rkvdec_ctx *ctx;

	rkvdec = container_of(to_delayed_work(work), struct rkvdec_dev,
			      watchdog_work);
	ctx = v4l2_m2m_get_curr_priv(rkvdec->m2m_dev);
	/*
	 * Link mode finishes the m2m job at submit (model flip), so
	 * get_curr_priv() is NULL by the time we fire. Fall back to the
	 * stashed completion-owner ctx. (LINK_MODE_PORT_DESIGN_2026-06-01 §3)
	 */
	if (!ctx && rkvdec_link_mode)
		ctx = rkvdec->link_ctx;
	if (ctx) {
		enum vb2_buffer_state state = VB2_BUF_STATE_ERROR;
		bool silent_completion = false;

		/*
		 * Link mode (BSP-faithful, 2026-06-01): the threaded IRQ
		 * normally reaps via the per-slot descriptor writeback. The
		 * watchdog is the fallback for a missed IRQ. Reap whatever HW
		 * has finished; if tasks remain, reschedule the poll until the
		 * budget runs out, then soft-reset + force-ERROR the stuck
		 * remainder so the next frame's IRQ is delivered.
		 */
		if (rkvdec_link_mode && ctx->link_table) {
			u32 reaped = rkvdec_link_reap_writeback(ctx);

			if (rkvdec_inflight_depth(ctx) == 0)
				return;

			if (!reaped && ctx->watchdog_poll_count <
			    RKVDEC_MAX_WATCHDOG_POLLS) {
				ctx->watchdog_poll_count++;
				rkvdec_schedule_watchdog_poll(rkvdec);
				return;
			}
			if (reaped) {
				/* progress this fire; keep polling the rest */
				ctx->watchdog_poll_count = 0;
				rkvdec_schedule_watchdog_poll(rkvdec);
				return;
			}

			/*
			 * Budget exhausted, no writeback: stuck task / ring-
			 * runaway. BSP-model recovery (NEXT_SESSION_BSP_MODEL_PORT):
			 * quiesced reset + RESEND the stuck task (re-kick its
			 * still-filled descriptor slot), up to RKVDEC_MAX_LINK_RESETS
			 * times, so the session RECOVERS instead of cascading
			 * whole-session. The descriptor for the inflight tail is
			 * untouched, so a bootstrap re-kick after the reset retries
			 * exactly that frame. Only force-ERROR once the reset budget
			 * is spent.
			 */
			if (ctx->link_resets < RKVDEC_MAX_LINK_RESETS) {
				int slot = rkvdec_inflight_peek_tail_slot(ctx);

				ctx->link_resets++;
				rkvdec->telem_real_timeout++;
				pr_info("rkvdec-vp9: link stuck, quiesced reset+resend slot=%d (reset %u/%u)\n",
					slot, ctx->link_resets,
					RKVDEC_MAX_LINK_RESETS);
				rkvdec_link_quiesced_reset(rkvdec, ctx);
				if (slot >= 0)
					rkvdec_link_enqueue_vdpu383(
						ctx->link_table, (u32)slot, 1,
						rkvdec->link);
				ctx->watchdog_poll_count = 0;
				rkvdec_schedule_watchdog_poll(rkvdec);
				return;
			}

			/* reset budget spent: give up on this task */
			rkvdec->telem_real_timeout++;
			rkvdec_link_reap(ctx, VB2_BUF_STATE_ERROR);
			return;
		}

		/* Step A v2: silent-completion fast poll.
		 *
		 * Telemetry showed ~all VP9 link-mode tasks silently
		 * complete (HW clears DEC_ENABLE, never raises STA_INT,
		 * no IRQ). Detect them at 10 ms granularity instead of
		 * waiting the full 80 ms watchdog timeout.
		 *
		 * If HW is still mid-decode (dec_en==1 and irq_sta==0),
		 * just reschedule and bail WITHOUT touching any HW state.
		 * Reading dec_en and irq_sta is side-effect-free; the
		 * disable-IRQ / clear-STA_INT / soft-reset writes are
		 * still under the dec_en==0 branch below.
		 *
		 * Bounded by ctx->watchdog_poll_count so a genuine HW
		 * stall still gets the full timeout escalation.
		 *
		 * 2026-05-28 fix: extended to single-shot mode too. The
		 * 10 ms watchdog was racing single-shot HW decodes
		 * (typical 5-15 ms), causing watchdog to fire before
		 * IRQ-driven completion - the race manifested as 100+
		 * real_to events per BBB200 (was 4.5 s before; 14-20 s
		 * with watchdog winning the race). Polling fast path
		 * is now mode-agnostic - dec_en register exists for
		 * both single-shot and link decode paths. */
		if (rkvdec->link) {
			u32 dec_en = readl(rkvdec->link +
					   VDPU383_LINK_DEC_ENABLE);
			u32 irq_sta = readl(rkvdec->regs +
					    VDPU383_OFFSET_COMMON_REGS + 7 * 4);

			/*
			 * NB (LINK_MODE_PORT_DESIGN_2026-06-01 §3, 2026-06-01
			 * test): in link mode neither dec_en (0x40, unused) nor
			 * LINK_DEC_NUM (0x10, never advances on this setup) is a
			 * working completion signal — HW decodes silently. A
			 * dec_num-based reschedule was tried and only added 80 ms
			 * latency per frame with no correctness gain, so we keep
			 * the dec_en check: in link mode it reads 0, falls
			 * straight through to the silent-completion branch below.
			 * Link-mode decode CORRECTNESS (wrong pixels) is the
			 * separate R2/R3 blocker, not a completion-detection one.
			 */
			if (dec_en != 0 && irq_sta == 0 &&
			    ctx->watchdog_poll_count <
			    RKVDEC_MAX_WATCHDOG_POLLS) {
				ctx->watchdog_poll_count++;
				rkvdec_schedule_watchdog_poll(rkvdec);
				return;
			}
		}

		writel(RKVDEC_IRQ_DIS, rkvdec->regs + RKVDEC_REG_INTERRUPT);

		/* VDPU383 link-mode: before declaring timeout, re-read the
		 * hardware status registers.  Discovered silent-completion
		 * mode: hardware finishes the decode, clears DEC_ENABLE on
		 * completion, but no IRQ ever fires.  By the time the watchdog
		 * runs, dec_en reads 0x0, sta is 0x0, no error bits set.
		 *
		 * Recovery strategy on watchdog fire (VDPU383 only):
		 *   1. status has DEC_RDY_STA          → late IRQ; success
		 *   2. status is BIT(2) + reg15 clean  → late alt-path; success
		 *   3. status == 0 AND dec_en == 0     → silent completion; success
		 *   4. otherwise                        → real timeout */
		if (rkvdec->link) {
			u32 status = readl(rkvdec->link + VDPU383_LINK_STA_INT);

			/*
			 * Watchdog-time decode-status check. On VDPU383 the IP
			 * block has three benign paths that look like timeouts
			 * to the SW watchdog because no IRQ ever fires:
			 *   - DEC_RDY_STA set: HW completed normally, the IRQ
			 *     was lost on the AXI bus.
			 *   - status == BIT(2) and irq_sta == 0: alt completion
			 *     path; buffer contains decoded data.
			 *   - status == 0 and DEC_ENABLE == 0: silent completion;
			 *     buffer contains decoded data.
			 * In the last two cases we still need to soft-reset the
			 * IP block so the next decode's IRQ is delivered, but
			 * the buffer is presentable.
			 */
			if (status & VDPU383_STA_INT_DEC_RDY_STA) {
				state = VB2_BUF_STATE_DONE;
			} else {
				/*
				 * Generalised silent-completion check (Phase 3
				 * v0.3 step 2 followup): the real signal that HW
				 * finished cleanly is `irq_sta == 0` (no error
				 * bits in codec reg15) AND `dec_en == 0` (the
				 * DEC_ENABLE register cleared itself, which HW
				 * does on completion). When both are zero, the
				 * decoded buffer is presentable regardless of
				 * which LINK_STA_INT bits happen to be latched.
				 *
				 * The previous code only matched LINK_STA_INT ==
				 * BIT(2) or LINK_STA_INT == 0, missing observed
				 * status values 0x800 / 0x802 / 0x804 (BIT(11)
				 * alone or combined with BIT(1)/BIT(2)) on
				 * 1080p VP9 INTER content. Those cascaded into
				 * gst flow errors -5 even though HW had finished
				 * cleanly.
				 *
				 * On true HW timeouts irq_sta has at least one
				 * error bit set (strm_err / bus_err / core_TO /
				 * etc.) OR dec_en is still 1 (HW stuck running),
				 * so we leave state=ERROR.
				 */
				u32 irq_sta = readl(rkvdec->regs +
					VDPU383_OFFSET_COMMON_REGS + 7 * 4);
				u32 dec_en = readl(rkvdec->link +
					VDPU383_LINK_DEC_ENABLE);

				if (irq_sta == 0 && dec_en == 0) {
					/*
					 * 2026-05-31 Round 11: discriminate REAL
					 * silent-completion (HW finished but lost
					 * IRQ on AXI bus) from FAKE silent-
					 * completion (HW gave up partway and just
					 * stopped). Use LINK_DEC_NUM advance as the
					 * authoritative HW completion signal.
					 *
					 * Real: irq_sta==0 && dec_en==0 && dec_num
					 *       advanced past expected → DONE
					 * Fake: irq_sta==0 && dec_en==0 && dec_num
					 *       NOT advanced → partial decode → ERROR
					 *
					 * Round 11.1 fix: in the FAKE case, we MUST
					 * still set silent_completion=true. That
					 * causes the downstream "if (state == ERROR
					 * && !silent_completion) iommu_restore"
					 * check to skip the IOMMU mangling. On the
					 * partial-decode case HW is in some weird
					 * intermediate state and yanking the IOMMU
					 * (via empty-domain attach/detach) wedges
					 * the device. The soft-reset below still
					 * runs because it's gated on
					 * `silent_completion || state == ERROR`
					 * which is true either way.
					 *
					 * Without this check, the LM 03-deltaq dump
					 * comparison (Tier 2 finding) showed the
					 * silent-completion buffer had only 10 Y rows
					 * decoded then descriptor-residue garbage
					 * past that. HW had abandoned the decode
					 * but our code marked DONE → MD5 mismatch
					 * → -64 vector LM regression on 03-size.
					 */
					u32 completed =
						ctx->link_table ?
						rkvdec_link_check_completion(
							ctx->link_table,
							rkvdec->link) : 1;
					/* Single-shot (AV1/HEVC/H264) has no link table;
					 * completed=1 -> genuine silent completion. The
					 * link calls below NULL-deref in single-shot and
					 * oops the watchdog worker (AV1 D-state hang). */
					/*
					 * 2026-05-31 Round 12 v2 telemetry:
					 * classify each silent_completion by what
					 * tb_reg[tb_reg_int] and DEC_NUM say,
					 * WITHOUT changing the existing Round 11.1
					 * pass/fail decision. The breakdown drives
					 * the next iteration's discriminator
					 * design.
					 */
					{
						u32 ts = ctx->link_table ?
							rkvdec_link_read_task_status(
								ctx->link_table, 0) : 0;
						bool wb = (ts != 0xffffffffu) &&
							  (ts != 0);
						bool err_bits = wb && (ts & 0x3feu);

						if (wb && !err_bits)
							rkvdec->telem_silent_clean_wb++;
						else if (wb)
							rkvdec->telem_silent_err_wb++;
						else if (completed > 0)
							rkvdec->telem_silent_dec_only++;
						else
							rkvdec->telem_silent_no_wb++;

						pr_info_ratelimited(
							"rkvdec silent: status=0x%08x dec_num+=%u wb=%d err=%d (no behavior change)\n",
							ts, completed, wb, err_bits);
					}
					silent_completion = true;
					rkvdec->telem_silent_completion++;
					if (completed > 0) {
						state = VB2_BUF_STATE_DONE;
						pr_debug("rkvdec: silent-completion sta=0x%08x irq_sta=0 dec_en=0 dec_num advanced (REAL)\n",
							 status);
					} else {
						/* HW didn't advance dec_num →
						 * partial decode → fake silent.
						 * Leave state == ERROR but keep
						 * silent_completion=true so
						 * iommu_restore is skipped. */
						pr_debug("rkvdec: FAKE silent-completion (dec_num didn't advance) → ERROR\n");
					}
				}
			}

			if (state == VB2_BUF_STATE_ERROR) {
				u32 irq_sta = readl(rkvdec->regs +
					VDPU383_OFFSET_COMMON_REGS + 7 * 4);
				u32 dec_en = readl(rkvdec->link +
					VDPU383_LINK_DEC_ENABLE);

				rkvdec->telem_real_timeout++;
				dev_err(rkvdec->dev,
					"Frame processing timed out (sta=0x%08x irq_sta=0x%08x dec_en=0x%08x)!\n",
					status, irq_sta, dec_en);
			}

			/* Disable VDPU383 link IRQs and clear pending status so
			 * a stale IRQ from this decode doesn't confuse the next
			 * frame's ISR. */
			writel(FIELD_PREP_WM16(VDPU383_INT_EN_IRQ |
					       VDPU383_INT_EN_LINE_IRQ, 0),
			       rkvdec->link + VDPU383_LINK_INT_EN);
			writel(FIELD_PREP_WM16(VDPU383_STA_INT_ALL, 0),
			       rkvdec->link + VDPU383_LINK_STA_INT);

			/*
			 * Soft-reset the IP block on ANY error path (silent OR
			 * real timeout). Without this on real-timeout content
			 * (e.g. vp90-2-08-tile_1x?_frame_parallel) HW stays in
			 * DEC_E=1 with active DMA outstanding — and the
			 * rkvdec_iommu_restore() below yanks IOMMU mappings
			 * mid-DMA, producing "page fault while iommu not
			 * attached to domain" kernel oops on a subsequent
			 * test in the same module load. Doing soft-reset first
			 * stops HW so the IOMMU manipulation is safe.
			 *
			 * Reset sequence (7 writes: enable bypass, trigger
			 * reset, poll for completion, clear status, restore
			 * enable) matches the vendor driver.
			 */
			if (silent_completion || state == VB2_BUF_STATE_ERROR) {
				u32 ip_en = readl(rkvdec->link +
						  VDPU383_LINK_IP_ENABLE);
				int poll;

				writel(ip_en | BIT(15),
				       rkvdec->link + VDPU383_LINK_IP_ENABLE);
				writel(BIT(0), rkvdec->link + 0x44);
				for (poll = 0; poll < 50; poll++) {
					if (readl(rkvdec->link +
						  VDPU383_LINK_STA_INT) &
					    BIT(11))
						break;
					udelay(10);
				}
				writel(FIELD_PREP_WM16(BIT(11), 0),
				       rkvdec->link + VDPU383_LINK_STA_INT);
				writel(FIELD_PREP_WM16(0xffff, 0),
				       rkvdec->link + VDPU383_LINK_INT_EN);
				writel(FIELD_PREP_WM16(VDPU383_STA_INT_ALL, 0),
				       rkvdec->link + VDPU383_LINK_STA_INT);
				writel(ip_en,
				       rkvdec->link + VDPU383_LINK_IP_ENABLE);
				/*
				 * Re-arm frame-done IRQ so the next frame can
				 * fire DEC_RDY normally instead of cascading
				 * through the watchdog. The soft-reset above
				 * clears STA_INT, so the re-arm is safe — any
				 * stale bit-2 would otherwise produce a
				 * spurious-IRQ storm.
				 */
				writel(FIELD_PREP_WM16(VDPU383_INT_EN_IRQ,
						       VDPU383_INT_EN_IRQ),
				       rkvdec->link + VDPU383_LINK_INT_EN);

				/*
				 * R41 (2026-06-01) — re-run warmup descriptor
				 * after soft-reset to re-establish IP state
				 * that VP9 alt-ref decode appears to need.
				 *
				 * Empirical pattern (R40): the first ~7
				 * frames after fresh insmod decode cleanly
				 * (probe warmup is active state). Once a
				 * hang happens, the soft-reset alone doesn't
				 * restore that state, and every subsequent
				 * frame hangs too.
				 *
				 * Module-param gated for A/B.
				 */
				if (r41_warmup_after_reset &&
				    rkvdec->link &&
				    rkvdec->rk3576_warmup_cpu) {
					rkvdec_rk3576_warmup_run(
						rkvdec->link,
						rkvdec->rk3576_warmup_dma);
				}
			}
		} else {
			dev_err(rkvdec->dev, "Frame processing timed out!\n");
		}

		/* Restore IOMMU on real HW errors only (silent_completion
		 * already recovers via soft-reset, no IOMMU yank needed).
		 * Now safe to call because the soft-reset above stopped HW
		 * DMA before we change the IOMMU attach state. */
		if (state == VB2_BUF_STATE_ERROR && !silent_completion)
			rkvdec_iommu_restore(rkvdec);

		/*
		 * Link mode: the m2m job was finished at submit; reap the
		 * parked buffers + drop the per-task pm ref + try_schedule.
		 * Single-shot: the classic finish (pm_put + buf_done +
		 * job_finish) — unchanged.
		 */
		if (rkvdec_link_mode && ctx->link_table) {
			if ((u32)rkvdec_link_depth > 1) {
				/*
				 * 2026-06-07 depth>1 completion. The global
				 * DEC_RDY_STA bit + whole-ring rkvdec_link_reap
				 * returns EVERY in-flight buffer the instant ANY
				 * frame signals done, so a later frame HW is still
				 * writing gets captured half-decoded (observed:
				 * frame correct to ~row 588, rest zero). Instead reap
				 * ONLY slots whose own per-slot writeback (tb_reg_int)
				 * is set, and leave the rest in flight + re-poll, so a
				 * frame is never returned before HW finished it.
				 * Bounded by the reset budget so a genuinely stuck
				 * slot can't stall forever.
				 */
				u32 reaped = rkvdec_link_reap_writeback(ctx);
				u32 left = rkvdec_inflight_depth(ctx);

				pr_info_ratelimited("rkvdec h264-link d>1: reaped=%u left=%u resets=%u\n",
						    reaped, left, ctx->link_resets);
				if (left > 0) {
					if (++ctx->link_resets < RKVDEC_MAX_LINK_RESETS) {
						rkvdec_schedule_watchdog_poll(rkvdec);
					} else {
						rkvdec_link_quiesced_reset(rkvdec, ctx);
						rkvdec_link_reap(ctx, VB2_BUF_STATE_ERROR);
						ctx->link_resets = 0;
					}
				} else {
					ctx->link_resets = 0;
				}
			} else {
				rkvdec_link_reap(ctx, state);
			}
		} else {
			rkvdec_job_finish(ctx, state);
		}
	}
}

/*
 * Some SoCs, like RK3588 have multiple identical VDPU cores, but the
 * kernel is currently missing support for multi-core handling. Exposing
 * separate devices for each core to userspace is bad, since that does
 * not allow scheduling tasks properly (and creates ABI). With this workaround
 * the driver will only probe for the first core and early exit for the other
 * cores. Once the driver gains multi-core support, the same technique
 * for detecting the first core can be used to cluster all cores together.
 */
static int rkvdec_disable_multicore(struct rkvdec_dev *rkvdec)
{
	struct device_node *node = NULL;
	const char *compatible;
	bool is_first_core;
	int ret;

	/* Intentionally ignores the fallback strings */
	ret = of_property_read_string(rkvdec->dev->of_node, "compatible", &compatible);
	if (ret)
		return ret;

	/* The first compatible and available node found is considered the main core */
	do {
		node = of_find_compatible_node(node, NULL, compatible);
		if (of_device_is_available(node))
			break;
	} while (node);

	if (!node)
		return -EINVAL;

	is_first_core = (rkvdec->dev->of_node == node);

	of_node_put(node);

	if (!is_first_core) {
		dev_info(rkvdec->dev, "missing multi-core support, ignoring this instance\n");
		return -ENODEV;
	}

	return 0;
}

static const struct rkvdec_variant_ops rk3399_variant_ops = {
	.irq_handler = rk3399_irq_handler,
	.colmv_size = rkvdec_colmv_size,
	.flatten_matrices = transpose_and_flatten_matrices,
};

static const struct rkvdec_variant rk3288_rkvdec_variant = {
	.num_regs = 68,
	.coded_fmts = rk3288_coded_fmts,
	.num_coded_fmts = ARRAY_SIZE(rk3288_coded_fmts),
	.ops = &rk3399_variant_ops,
	.has_single_reg_region = true,
};

static const struct rkvdec_variant rk3328_rkvdec_variant = {
	.num_regs = 109,
	.coded_fmts = rkvdec_coded_fmts,
	.num_coded_fmts = ARRAY_SIZE(rkvdec_coded_fmts),
	.ops = &rk3399_variant_ops,
	.has_single_reg_region = true,
	.quirks = RKVDEC_QUIRK_DISABLE_QOS,
};

static const struct rkvdec_variant rk3399_rkvdec_variant = {
	.num_regs = 78,
	.coded_fmts = rkvdec_coded_fmts,
	.num_coded_fmts = ARRAY_SIZE(rkvdec_coded_fmts),
	.ops = &rk3399_variant_ops,
	.has_single_reg_region = true,
};

static const struct rcb_size_info vdpu381_rcb_sizes[] = {
	{6,	PIC_WIDTH,	0},	// intrar
	{1,	PIC_WIDTH,	0},	// transdr (Is actually 0.4*pic_width)
	{1,	PIC_HEIGHT,	0},	// transdc (Is actually 0.1*pic_height)
	{3,	PIC_WIDTH,	0},	// streamdr
	{6,	PIC_WIDTH,	0},	// interr
	{3,	PIC_HEIGHT,	0},	// interc
	{22,	PIC_WIDTH,	0},	// dblkr
	{6,	PIC_WIDTH,	0},	// saor
	{11,	PIC_WIDTH,	0},	// fbcr
	{67,	PIC_HEIGHT,	0},	// filtc col
};

static const struct rkvdec_variant_ops vdpu381_variant_ops = {
	.irq_handler = vdpu381_irq_handler,
	.colmv_size = rkvdec_colmv_size,
	.flatten_matrices = transpose_and_flatten_matrices,
};

static const struct rkvdec_variant vdpu381_variant = {
	.coded_fmts = vdpu381_coded_fmts,
	.num_coded_fmts = ARRAY_SIZE(vdpu381_coded_fmts),
	.rcb_sizes = vdpu381_rcb_sizes,
	.num_rcb_sizes = ARRAY_SIZE(vdpu381_rcb_sizes),
	.ops = &vdpu381_variant_ops,
};

/*
 * VDPU383 RCB scratch buffer sizes.
 *
 * The {multiplier, axis} fields scale with picture dimension; the min_bytes
 * floor reflects MPP's per-stage constant allocation for VP9 (and AV1) on
 * tiny content, where the multiplier-based formula under-provisions and the
 * decoder corrupts INTER frames at the filter stage. Empirically derived
 * from MPP's vdpu383_vp9d_rcb_calc() at width=64,height=64,single-tile,
 * NV12 8-bit (2026-05-29 regs_full.dat diff):
 *   inter_in_row   = 320 B   (fltd_row_append-style baseline)
 *   inter_on_row   = 320 B
 *   intra_in_row   = 192 B
 *   intra_on_row   = 192 B
 *   filterd_in_row = 15552 B (16 KB-class baseline, dominates up to 4K width)
 *   filterd_prot   = 15552 B
 *   filterd_tile_row = 3392 B
 *   filterd_tile_col = 3456 B  (height-axis)
 *
 * Rounded up to safe page-multiples. HEVC was using these slots fine with
 * the multiplier-only formula because HEVC content widths in the tested
 * Fluster suite never went small enough to fall below MPP's baseline.
 */
static const struct rcb_size_info vdpu383_rcb_sizes[] = {
	{6,	PIC_WIDTH,	0},		// streamd          (unused by VP9)
	{6,	PIC_WIDTH,	0},		// streamd_tile     (unused by VP9)
	{12,	PIC_WIDTH,	4096},		// inter
	{12,	PIC_WIDTH,	4096},		// inter_tile
	{16,	PIC_WIDTH,	4096},		// intra
	{10,	PIC_WIDTH,	4096},		// intra_tile
	{120,	PIC_WIDTH,	16384},		// filterd
	{120,	PIC_WIDTH,	16384},		// filterd_protect
	{120,	PIC_WIDTH,	4096},		// filterd_tile_row
	{180,	PIC_HEIGHT,	4096},		// filterd_tile_col
	{180,	PIC_HEIGHT,	0},		// filterd_av1_upscale_tile_col (AV1 only)
};

static const struct rkvdec_variant_ops vdpu383_variant_ops = {
	.irq_handler = vdpu383_irq_handler,
	.colmv_size = vdpu383_colmv_size,
	.hor_align = vdpu383_hor_align,
	.flatten_matrices = vdpu383_flatten_matrices,
};

static const struct rkvdec_variant vdpu383_variant = {
	.coded_fmts = vdpu383_coded_fmts,
	.num_coded_fmts = ARRAY_SIZE(vdpu383_coded_fmts),
	.rcb_sizes = vdpu383_rcb_sizes,
	.num_rcb_sizes = ARRAY_SIZE(vdpu383_rcb_sizes),
	.ops = &vdpu383_variant_ops,
};

static const struct of_device_id of_rkvdec_match[] = {
	{
		.compatible = "rockchip,rk3288-vdec",
		.data = &rk3288_rkvdec_variant,
	},
	{
		.compatible = "rockchip,rk3328-vdec",
		.data = &rk3328_rkvdec_variant,
	},
	{
		.compatible = "rockchip,rk3399-vdec",
		.data = &rk3399_rkvdec_variant,
	},
	{
		.compatible = "rockchip,rk3588-vdec",
		.data = &vdpu381_variant,
	},
	{
		.compatible = "rockchip,rk3576-vdec",
		.data = &vdpu383_variant,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_rkvdec_match);

static int rkvdec_probe(struct platform_device *pdev)
{
	const struct rkvdec_variant *variant;
	struct rkvdec_dev *rkvdec;
	int ret, irq;

	variant = of_device_get_match_data(&pdev->dev);
	if (!variant)
		return -EINVAL;

	rkvdec = devm_kzalloc(&pdev->dev, sizeof(*rkvdec), GFP_KERNEL);
	if (!rkvdec)
		return -ENOMEM;

	platform_set_drvdata(pdev, rkvdec);
	rkvdec->dev = &pdev->dev;
	rkvdec->variant = variant;
	mutex_init(&rkvdec->vdev_lock);
	INIT_DELAYED_WORK(&rkvdec->watchdog_work, rkvdec_watchdog_func);

	ret = rkvdec_disable_multicore(rkvdec);
	if (ret)
		return ret;

	ret = devm_clk_bulk_get_all_enabled(&pdev->dev, &rkvdec->clocks);
	if (ret < 0)
		return ret;

	rkvdec->num_clocks = ret;
	rkvdec->axi_clk = devm_clk_get(&pdev->dev, "axi");

	if (rkvdec->variant->has_single_reg_region) {
		rkvdec->regs = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(rkvdec->regs))
			return PTR_ERR(rkvdec->regs);
	} else {
		rkvdec->regs = devm_platform_ioremap_resource_byname(pdev, "function");
		if (IS_ERR(rkvdec->regs))
			return PTR_ERR(rkvdec->regs);

		rkvdec->link = devm_platform_ioremap_resource_byname(pdev, "link");
		if (IS_ERR(rkvdec->link))
			return PTR_ERR(rkvdec->link);
	}

	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Could not set DMA coherent mask.\n");
		return ret;
	}

	vb2_dma_contig_set_max_seg_size(&pdev->dev, DMA_BIT_MASK(32));

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return -ENXIO;

	INIT_WORK(&rkvdec->irq_done_work, rkvdec_irq_done_work_fn);

	/*
	 * Primary hardirq = rkvdec_irq_top (returns IRQ_WAKE_THREAD when the
	 * irq_split experiment is off, so behaviour is identical to the old
	 * NULL-primary registration); threaded = rkvdec_irq_handler.
	 */
	/* 2026-06-08 Variant B: init the IRQ-driven-warmup completion before the
	 * IRQ goes live (warmup_irq_inflight is already false via kzalloc). */
	init_completion(&rkvdec->warmup_irq_done);

	ret = devm_request_threaded_irq(&pdev->dev, irq, rkvdec_irq_top,
					rkvdec_irq_handler, IRQF_ONESHOT,
					dev_name(&pdev->dev), rkvdec);
	rkvdec->irq = irq;
	if (ret) {
		dev_err(&pdev->dev, "Could not request vdec IRQ\n");
		return ret;
	}

	/*
	 * vdpu383-submit Phase 1a: resident kthread worker for link-mode
	 * completion (rkvdec_submit_mode=2). VDPU383-only (the sole variant with
	 * link mode). Best-effort: on failure link_worker stays NULL and
	 * submit_mode=2 transparently falls back to the threaded-IRQ reap.
	 */
	if (rkvdec->variant == &vdpu383_variant && rkvdec->link) {
		kthread_init_work(&rkvdec->link_reap_work,
				  rkvdec_link_reap_work_fn);
		rkvdec->link_worker =
			kthread_create_worker(0, "rkvdec-link/%s",
					      dev_name(&pdev->dev));
		if (IS_ERR(rkvdec->link_worker)) {
			dev_warn(&pdev->dev,
				 "link kthread worker create failed: %ld (submit_mode=2 unavailable)\n",
				 PTR_ERR(rkvdec->link_worker));
			rkvdec->link_worker = NULL;
		}
	}

	rkvdec->sram_pool = of_gen_pool_get(pdev->dev.of_node, "sram", 0);
	if (!rkvdec->sram_pool && rkvdec->variant->num_rcb_sizes > 0)
		dev_info(&pdev->dev, "No sram node, RCB will be stored in RAM\n");

	pm_runtime_set_autosuspend_delay(&pdev->dev, 100);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = rkvdec_v4l2_init(rkvdec);
	if (ret)
		goto err_disable_runtime_pm;

	rkvdec->iommu_domain = iommu_get_domain_for_dev(&pdev->dev);
	if (rkvdec->iommu_domain) {
		rkvdec->empty_domain = iommu_paging_domain_alloc(rkvdec->dev);

		if (IS_ERR(rkvdec->empty_domain)) {
			rkvdec->empty_domain = NULL;
			dev_warn(rkvdec->dev, "cannot alloc new empty domain\n");
		}

		/* Silence IOMMU fault spam from rk_iommu's default handler so
		 * dmesg has room for our diagnostic prints. We just count and
		 * log first occurrence with rate limit. */
		iommu_set_fault_handler(rkvdec->iommu_domain,
					rkvdec_silent_iommu_fault, rkvdec);
	}

	/*
	 * 2026-06-01 R23 — RK3576 HW warmup. The BSP runs this once at
	 * rkvdec2_probe_default (mpp_rkvdec2.c:2057) via
	 * dec->hw_ops->hack_run = rk3576_workaround_run. Without it the IP
	 * comes up in an indeterminate state that works for non-tile VP9
	 * content but fails on column-tile content (R17/R18/R20/R22:
	 * every input data byte we hand HW is byte-identical to MPP yet
	 * tile decode fails until the warmup runs). See
	 * SESSION_2026-06-01_R22_PROB_DIFF.md and the kcore disassembly in
	 * .local/workaround_*.S for the discovery + port trail.
	 *
	 * Only run on the VDPU383 variant (RK3576). Earlier SoCs (rk3399,
	 * rk3328, rk3568, rk3588 = vdpu381) use different IP blocks that
	 * don't need this workaround.
	 */
	if (rkvdec->variant == &vdpu383_variant && rkvdec->link && !warmup_off) {
		int rc;

		ret = pm_runtime_resume_and_get(&pdev->dev);
		if (ret < 0) {
			dev_warn(&pdev->dev,
				 "rk3576 warmup: pm_runtime_resume failed: %d\n",
				 ret);
			ret = 0;
			goto warmup_skip;
		}

		ret = rkvdec_rk3576_warmup_alloc(&pdev->dev,
						 &rkvdec->rk3576_warmup_cpu,
						 &rkvdec->rk3576_warmup_dma);
		if (ret) {
			dev_warn(&pdev->dev,
				 "rk3576 warmup: alloc failed: %d\n", ret);
			pm_runtime_put_autosuspend(&pdev->dev);
			ret = 0;
			goto warmup_skip;
		}

		rc = warmup_irq ?
		     rkvdec_rk3576_warmup_run_irq(rkvdec) :
		     rkvdec_rk3576_warmup_run(rkvdec->link,
					      rkvdec->rk3576_warmup_dma);
		if (rc == 0) {
			dev_info(&pdev->dev,
				 "rk3576 warmup: HW init OK\n");
		} else if (rc == -EIO) {
			dev_warn(&pdev->dev,
				 "rk3576 warmup: HW reported error (status err bits)\n");
		} else if (rc == -ETIMEDOUT) {
			dev_warn(&pdev->dev,
				 "rk3576 warmup: HW did not complete\n");
		}
		/* Either way, the cleanup writes at end of warmup_run leave
		 * link registers in a known-good zero state — same as a
		 * fresh boot. Continue with probe regardless. */

		pm_runtime_put_autosuspend(&pdev->dev);
	}
warmup_skip:

	/*
	 * Bug-A dmaengine phase 1: request a DMA_MEMCPY-capable channel
	 * (any PL330 controller on this SoC will do; we just need
	 * hardware-to-hardware memcpy support). Channel held for the
	 * lifetime of the driver; released in remove().
	 */
	if (vp9_bug_a_phase >= 1) {
		dma_cap_mask_t mask;

		dma_cap_zero(mask);
		dma_cap_set(DMA_MEMCPY, mask);
		vp9_bug_a_chan = dma_request_chan_by_mask(&mask);
		if (IS_ERR(vp9_bug_a_chan)) {
			dev_warn(&pdev->dev,
				 "bug-a phase 1: dma_request_chan_by_mask(DMA_MEMCPY) failed: %ld\n",
				 PTR_ERR(vp9_bug_a_chan));
			vp9_bug_a_chan = NULL;
		} else {
			dev_info(&pdev->dev,
				 "bug-a phase 1: acquired DMA_MEMCPY channel '%s' on '%s'\n",
				 dma_chan_name(vp9_bug_a_chan),
				 dev_name(vp9_bug_a_chan->device->dev));
		}
	}

	/*
	 * Bug-A dmaengine phase 2: prove the PL330 channel can actually
	 * transfer between two physical buffers. Uses dma_alloc_coherent
	 * (kernel-managed, IRQ-safe, IOVA = phys on systems without an
	 * IOMMU between PL330 and DRAM). No V4L2 buffer involvement, so
	 * if this fails we know the channel itself is broken and don't
	 * need to chase IOMMU domain bridging.
	 */
	if (vp9_bug_a_phase >= 2 && vp9_bug_a_chan) {
		const size_t test_len = 4096;
		dma_addr_t src_dma, dst_dma;
		void *src_cpu, *dst_cpu;
		struct dma_async_tx_descriptor *tx;
		dma_cookie_t cookie;
		enum dma_status status;
		int wait_ms = 100;

		src_cpu = dma_alloc_coherent(vp9_bug_a_chan->device->dev,
					     test_len, &src_dma, GFP_KERNEL);
		dst_cpu = dma_alloc_coherent(vp9_bug_a_chan->device->dev,
					     test_len, &dst_dma, GFP_KERNEL);
		if (!src_cpu || !dst_cpu) {
			dev_warn(&pdev->dev, "bug-a phase 2: dma_alloc_coherent failed\n");
			goto bug_a_phase2_done;
		}
		memset(src_cpu, 0xAB, test_len);
		memset(dst_cpu, 0x00, test_len);

		tx = dmaengine_prep_dma_memcpy(vp9_bug_a_chan, dst_dma,
					       src_dma, test_len,
					       DMA_PREP_INTERRUPT);
		if (!tx) {
			dev_warn(&pdev->dev, "bug-a phase 2: prep_dma_memcpy returned NULL\n");
			goto bug_a_phase2_free;
		}

		cookie = dmaengine_submit(tx);
		dma_async_issue_pending(vp9_bug_a_chan);

		/* Spin-wait up to wait_ms for the transfer to complete. */
		while (wait_ms-- > 0) {
			status = dma_async_is_tx_complete(vp9_bug_a_chan,
							  cookie, NULL, NULL);
			if (status == DMA_COMPLETE)
				break;
			mdelay(1);
		}

		if (status == DMA_COMPLETE) {
			u8 first = ((u8 *)dst_cpu)[0];
			u8 last  = ((u8 *)dst_cpu)[test_len - 1];

			dev_info(&pdev->dev,
				 "bug-a phase 2: PL330 memcpy %zu bytes OK (dst[0]=0x%02x dst[%zu]=0x%02x; expected 0xAB)\n",
				 test_len, first, test_len - 1, last);
		} else {
			dev_warn(&pdev->dev,
				 "bug-a phase 2: PL330 memcpy did not complete (status=%d)\n",
				 status);
		}

bug_a_phase2_free:
		dma_free_coherent(vp9_bug_a_chan->device->dev, test_len,
				  src_cpu, src_dma);
		dma_free_coherent(vp9_bug_a_chan->device->dev, test_len,
				  dst_cpu, dst_dma);
	}
bug_a_phase2_done:

	return 0;

err_disable_runtime_pm:
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	if (rkvdec->sram_pool)
		gen_pool_destroy(rkvdec->sram_pool);

	return ret;
}

static void rkvdec_remove(struct platform_device *pdev)
{
	struct rkvdec_dev *rkvdec = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&rkvdec->watchdog_work);
	cancel_work_sync(&rkvdec->irq_done_work); /* irq_split bottom-half */

	/* vdpu383-submit Phase 1a: tear down the link completion worker. */
	if (rkvdec->link_worker) {
		kthread_destroy_worker(rkvdec->link_worker);
		rkvdec->link_worker = NULL;
	}

	rkvdec_v4l2_cleanup(rkvdec);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);

	if (rkvdec->empty_domain)
		iommu_domain_free(rkvdec->empty_domain);

	if (rkvdec->rk3576_warmup_cpu)
		rkvdec_rk3576_warmup_free(&pdev->dev,
					  rkvdec->rk3576_warmup_cpu,
					  rkvdec->rk3576_warmup_dma);

	if (vp9_bug_a_chan) {
		dma_release_channel(vp9_bug_a_chan);
		vp9_bug_a_chan = NULL;
	}
}

#ifdef CONFIG_PM
static int rkvdec_runtime_resume(struct device *dev)
{
	struct rkvdec_dev *rkvdec = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(rkvdec->num_clocks, rkvdec->clocks);
	if (ret)
		return ret;

	/*
	 * R35 (2026-06-01) — re-run the BSP RK3576 HW warmup on every
	 * runtime_resume, not just once at probe.
	 *
	 * BSP rkvdec2_runtime_resume (mpp_rkvdec2.c:2188-2189) calls
	 * dec->hw_ops->hack_run after clk_on on every resume. Our R23
	 * port only ran the warmup at probe — so the very first decode
	 * after each autosuspend (100 ms idle gate) hits an unwarmed IP.
	 *
	 * Hypothesis matches the evidence pattern from R20–R34:
	 *   - mpi_dec_test runs at 194 fps continuously → autosuspend
	 *     never triggers → warmup state from probe is preserved
	 *     → decode succeeds.
	 *   - Our V4L2 path via gst has session-start and inter-frame
	 *     idle gaps > 100 ms → autosuspend fires → IP loses
	 *     warmed-up state → first decode after resume fails.
	 *
	 * Every register and every DMA buffer byte we hand HW has been
	 * proven byte-identical to MPP CLI (R20 / R22 / R34), so the
	 * remaining variable can only be HW state established by the
	 * warmup. This re-runs it.
	 *
	 * Guarded on the warmup buffer being allocated — defensive in
	 * case probe's warmup setup failed (we continue without it then).
	 */
	/* R35 diag — always count runtime_resume calls, separate from the
	 * conditional that controls whether we actually re-warmup. */
	rkvdec->r35_resume_total_calls++;

	if (r35_warmup_on_resume &&
	    rkvdec->variant == &vdpu383_variant &&
	    rkvdec->link && rkvdec->rk3576_warmup_cpu) {
		int rc = warmup_irq ?
			 rkvdec_rk3576_warmup_run_irq(rkvdec) :
			 rkvdec_rk3576_warmup_run(rkvdec->link,
						  rkvdec->rk3576_warmup_dma);
		rkvdec->r35_resume_warmups++;
		if (rc == 0)
			rkvdec->r35_resume_warmups_ok++;
		else if (rc == -EIO)
			rkvdec->r35_resume_warmups_eio++;
		else
			rkvdec->r35_resume_warmups_err++;
		if (rc && rc != -EIO)
			dev_warn_ratelimited(dev,
					     "rk3576 warmup on resume: %d\n",
					     rc);
	}

	return 0;
}

static int rkvdec_runtime_suspend(struct device *dev)
{
	struct rkvdec_dev *rkvdec = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(rkvdec->num_clocks, rkvdec->clocks);
	return 0;
}
#endif

static const struct dev_pm_ops rkvdec_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rkvdec_runtime_suspend, rkvdec_runtime_resume, NULL)
};

static struct platform_driver rkvdec_driver = {
	.probe = rkvdec_probe,
	.remove = rkvdec_remove,
	.driver = {
		   .name = "rkvdec",
		   .of_match_table = of_rkvdec_match,
		   .pm = &rkvdec_pm_ops,
	},
};
module_platform_driver(rkvdec_driver);

MODULE_AUTHOR("Boris Brezillon <boris.brezillon@collabora.com>");
MODULE_DESCRIPTION("Rockchip Video Decoder driver");
MODULE_LICENSE("GPL v2");
