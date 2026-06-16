/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip Video Decoder driver
 *
 * Copyright (C) 2019 Collabora, Ltd.
 *
 * Based on rkvdec driver by Google LLC. (Tomasz Figa <tfiga@chromium.org>)
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */
#ifndef RKVDEC_H_
#define RKVDEC_H_

#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/clk.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#define RKVDEC_QUIRK_DISABLE_QOS	BIT(0)

#define RKVDEC_1080P_PIXELS		(1920 * 1088)
#define RKVDEC_4K_PIXELS		(4096 * 2304)
#define RKVDEC_8K_PIXELS		(7680 * 4320)

struct rkvdec_ctx;
struct rkvdec_rcb_config;

struct rkvdec_ctrl_desc {
	struct v4l2_ctrl_config cfg;
};

struct rkvdec_ctrls {
	const struct rkvdec_ctrl_desc *ctrls;
	unsigned int num_ctrls;
};

struct rkvdec_run {
	struct {
		struct vb2_v4l2_buffer *src;
		struct vb2_v4l2_buffer *dst;
	} bufs;
};

struct rkvdec_vp9_decoded_buffer_info {
	/* Info needed when the decoded frame serves as a reference frame. */
	unsigned short width;
	unsigned short height;
	unsigned int bit_depth : 4;
};

struct rkvdec_decoded_buffer {
	/* Must be the first field in this struct. */
	struct v4l2_m2m_buffer base;

	union {
		struct rkvdec_vp9_decoded_buffer_info vp9;
	};
};

static inline struct rkvdec_decoded_buffer *
vb2_to_rkvdec_decoded_buf(struct vb2_buffer *buf)
{
	return container_of(buf, struct rkvdec_decoded_buffer,
			    base.vb.vb2_buf);
}

struct rkvdec_variant_ops {
	irqreturn_t (*irq_handler)(struct rkvdec_ctx *ctx);
	u32 (*colmv_size)(u16 width, u16 height);
	u32 (*hor_align)(u32 width);
	void (*flatten_matrices)(u8 *output, const u8 *input, int matrices, int row_length);
};

struct rkvdec_variant {
	unsigned int num_regs;
	const struct rkvdec_coded_fmt_desc *coded_fmts;
	size_t num_coded_fmts;
	const struct rcb_size_info *rcb_sizes;
	size_t num_rcb_sizes;
	const struct rkvdec_variant_ops *ops;
	bool has_single_reg_region;
	unsigned int quirks;
};

struct rkvdec_coded_fmt_ops {
	int (*adjust_fmt)(struct rkvdec_ctx *ctx,
			  struct v4l2_format *f);
	int (*start)(struct rkvdec_ctx *ctx);
	void (*stop)(struct rkvdec_ctx *ctx);
	int (*run)(struct rkvdec_ctx *ctx);
	void (*done)(struct rkvdec_ctx *ctx, struct vb2_v4l2_buffer *src_buf,
		     struct vb2_v4l2_buffer *dst_buf,
		     enum vb2_buffer_state result);
	int (*try_ctrl)(struct rkvdec_ctx *ctx, struct v4l2_ctrl *ctrl);
	enum rkvdec_image_fmt (*get_image_fmt)(struct rkvdec_ctx *ctx,
					       struct v4l2_ctrl *ctrl);

	/*
	 * Inspect a just-set V4L2 control and report whether the
	 * encoded frame dimensions it carries differ from the current
	 * `ctx->decoded_fmt`. When non-NULL and returning true, the
	 * core emits V4L2_EVENT_SOURCE_CHANGE so userspace knows to
	 * stop the CAPTURE queue, release buffers, query the new
	 * format, and restart. Stateless decoders parse the bitstream
	 * themselves so userspace already has the new dimensions; the
	 * event is a signal that the driver agrees and the buffer
	 * cycle should run now.
	 *
	 * Codec backends that haven't implemented this leave the
	 * pointer NULL — userspace must continue to size buffers
	 * correctly up-front, and mid-stream dimension changes will
	 * fail (current pre-existing behaviour).
	 */
	bool (*check_source_change)(struct rkvdec_ctx *ctx,
				    struct v4l2_ctrl *ctrl);
};

enum rkvdec_image_fmt {
	RKVDEC_IMG_FMT_ANY = 0,
	RKVDEC_IMG_FMT_420_8BIT,
	RKVDEC_IMG_FMT_420_10BIT,
	RKVDEC_IMG_FMT_422_8BIT,
	RKVDEC_IMG_FMT_422_10BIT,
};

struct rkvdec_decoded_fmt_desc {
	u32 fourcc;
	enum rkvdec_image_fmt image_fmt;
};

struct rkvdec_coded_fmt_desc {
	u32 fourcc;
	struct v4l2_frmsize_stepwise frmsize;
	const struct rkvdec_ctrls *ctrls;
	const struct rkvdec_coded_fmt_ops *ops;
	unsigned int num_decoded_fmts;
	const struct rkvdec_decoded_fmt_desc *decoded_fmts;
	u32 subsystem_flags;
};

struct rkvdec_dev {
	struct v4l2_device v4l2_dev;
	struct media_device mdev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct device *dev;
	struct clk_bulk_data *clocks;
	unsigned int num_clocks;
	struct clk *axi_clk;
	void __iomem *regs;
	void __iomem *link;
	struct mutex vdev_lock; /* serializes ioctls */
	struct delayed_work watchdog_work;
	/*
	 * Link-mode completion owner (LINK_MODE_PORT_DESIGN_2026-06-01 §3).
	 * device_run job_finishes at submit, so v4l2_m2m_get_curr_priv()
	 * returns NULL by the time the watchdog/IRQ fire — they have no
	 * "current job" to find ctx from. vp9_run stashes the submitting
	 * ctx here (single-stream endpoint: one ctx at a time); the
	 * completion paths read it instead of get_curr_priv. Cleared in
	 * vp9_stop. NULL outside link mode.
	 */
	struct rkvdec_ctx *link_ctx;
	/* Saved IRQ number for the quiesced link reset (disable_irq during
	 * reset). BSP-model port. */
	int irq;

	/*
	 * 2026-06-05 throughput experiment (irq_split module param). When the
	 * split is on, the hardirq top-half classifies + acks the IRQ but
	 * defers the sleeping completion (iommu_restore + the codec done()
	 * op's v4l2_ctrl_find + job_finish) to this high-priority work item,
	 * which runs in process context. Single-stream endpoint at depth=1, so
	 * one completion is in flight at a time and these scratch fields don't
	 * race. Inert when irq_split=0. See SESSION_2026-06-05_FINALISATION.md.
	 */
	struct work_struct irq_done_work;
	struct rkvdec_ctx *irq_done_ctx;
	enum vb2_buffer_state irq_done_state;
	bool irq_done_iommu_restore;

	/*
	 * vdpu383-submit PLAN Phase 1a (rkvdec_submit_mode=2): resident
	 * kthread worker that owns the link-mode completion (per-slot
	 * writeback reap) off the threaded-IRQ path. Created at probe,
	 * destroyed at remove. The hardirq top-half acks the link IRQ and
	 * kthread_queue_work()s link_reap_work; the worker runs the sleeping
	 * reap (done()/buf_done/job_finish) in its own process context. NULL
	 * if worker creation failed (submit_mode=2 then falls back to the
	 * threaded-IRQ reap, same as mode 0).
	 */
	struct kthread_worker *link_worker;
	struct kthread_work link_reap_work;

	/*
	 * 2026-06-05 throughput diagnostic (vp9_time module param). Measures
	 * pure HW decode time (kick -> DONE IRQ) to separate decode rate from
	 * gst-pipeline overhead. Stamped at the single-shot kick, consumed in
	 * the DONE IRQ; mean printed every 100 frames. Inert when vp9_time=0.
	 */
	ktime_t vp9_kick_kt;
	/* Hardirq-arrival stamp (set in the IRQ top-half, overwritten on every
	 * IRQ so the DONE hardirq wins over any earlier line IRQ). Splits the
	 * kick->DONE interval into kick->hardirq (pure HW decode) and
	 * hardirq->threaded (IRQ/scheduling latency) — the decisive
	 * silicon-vs-driver-overhead measurement. */
	ktime_t vp9_hardirq_kt;
	u64 vp9_dec_ns_sum;	/* sum of kick->hardirq (HW decode) */
	u64 vp9_irq_ns_sum;	/* sum of hardirq->threaded (IRQ latency) */
	u32 vp9_dec_ns_cnt;

	/*
	 * 2026-06-05 throughput fix: flush-only-after-restore. The single-shot
	 * kick used to flush the whole IOMMU TLB every frame (~1.3-2.5 ms);
	 * that is redundant because vb2 IOVAs are stable frame-to-frame. Set
	 * true on anything that can change mappings (iommu_restore, queue
	 * (re)start) and on init; the kick flushes + clears it. Steady-state
	 * decode then does no flush. See throughput_2026-06-05.md.
	 */
	bool iommu_needs_flush;
	struct gen_pool *sram_pool;
	struct iommu_domain *iommu_domain;
	struct iommu_domain *empty_domain;
	const struct rkvdec_variant *variant;

	/* Step A telemetry (2026-05-28): count completion paths per
	 * session to validate where the time goes. Incremented from
	 * IRQ + watchdog contexts; printed by stop_streaming, then
	 * reset by start_streaming. Plain u32 is fine — printk is
	 * the only reader and a missed update on update/read race
	 * is acceptable for telemetry. */
	u32 telem_irq_done_clean;
	u32 telem_irq_done_err;
	u32 telem_irq_nowb;
	u32 telem_silent_completion;
	u32 telem_real_timeout;
	/* Silent-completion investigation: count IRQs where the outer
	 * dispatcher saw LINK_DEC_NUM == expected_dec_num and returned
	 * IRQ_NONE (the "spurious / dec_num bouncing" path). Hypothesis:
	 * HW IS firing per-task IRQs but our completion check misses
	 * many because the LINK_DEC_NUM counter resets between frames
	 * on this silicon. */
	u32 telem_irq_spurious;
	/*
	 * 2026-05-31 Round 12 v2: silent_completion breakdown.
	 * Discriminate the four kinds of watchdog-time silent_completion
	 * (irq_sta==0 && dec_en==0) by what tb_reg[tb_reg_int] and
	 * LINK_DEC_NUM say:
	 *   silent_clean_wb: tb_reg has writeback, no err bits → DONE
	 *   silent_err_wb:   tb_reg has writeback with err bits → ERROR
	 *   silent_dec_only: no tb_reg writeback but dec_num advanced
	 *                    (HW completed but didn't write descriptor)
	 *   silent_no_wb:    no tb_reg writeback, no dec_num advance
	 *                    (HW gave up partway)
	 */
	u32 telem_silent_clean_wb;
	u32 telem_silent_err_wb;
	u32 telem_silent_dec_only;
	u32 telem_silent_no_wb;
	/* 2026-05-29: vp9_ref_dma timestamp-lookup telemetry.
	 * Hypothesis: vb2_find_buffer() returns NULL for small/non-
	 * standard content's previous-frame timestamps, causing fallback
	 * to dst_dma - HW then reads current buffer as ref → corrupted
	 * INTER. Path A diagnostic for 02-size cluster bug. Path B would
	 * sweep production-content variants instead. */
	u32 telem_ref_lookup_total;
	u32 telem_ref_lookup_fallback;

	/*
	 * Accumulation-bug instrumentation (2026-05-30 Round 2.2).
	 * These counters are device-level (not reset per session) so we
	 * can see whether some resource accumulates across vector
	 * boundaries within ONE Fluster invocation (the documented
	 * crash trigger). Printed at every vp9_stop, look for any
	 * pair whose counts diverge as the batch progresses.
	 */
	u32 accum_open;
	u32 accum_release;
	u32 accum_start_stream_out;
	u32 accum_stop_stream_out;
	u32 accum_start_stream_cap;
	u32 accum_stop_stream_cap;
	u32 accum_vp9_start;
	u32 accum_vp9_stop;
	u32 accum_frames_run;	/* R39+ per-frame index for hang-point investigation */
	u32 accum_rcb_alloc;
	u32 accum_rcb_free;
	/*
	 * 2026-05-30 Round 2.3: count entries to the rkvdec_device_run
	 * "rcb didn't validate, rebuild it" branch. On safe content this
	 * should fire EXACTLY ONCE per stream (the initial alloc). >1
	 * means mid-stream resolution change triggered free + realloc,
	 * which is the suspected race window with active HW DMA on
	 * 14-resize content.
	 */
	u32 accum_rcb_rebuild;
	/*
	 * 2026-05-30 Round 2.4: HW-busy-at-boundary detection. Reads
	 * VDPU383_LINK_DEC_ENABLE (0x40 BIT(0)) at strategic sync points.
	 * - busy_at_start: HW left enabled by PRIOR session (cross-vector
	 *   leak; would be the smoking gun for fp-tile silent-fail leaving
	 *   HW DMA in flight after our cleanup ran).
	 * - busy_at_stop_out: HW still enabled when output q stop_streaming
	 *   begins (would mean our drain didn't fully quiesce HW).
	 * - busy_at_vp9_stop: HW still enabled when vp9_stop runs (after
	 *   the inflight drain + force-complete). Same diagnostic point
	 *   as Round 2.1's failed soft-reset attempt — we only need to
	 *   know if it's ever non-zero on safe content.
	 */
	u32 accum_busy_at_start;
	u32 accum_busy_at_stop_out;
	u32 accum_busy_at_vp9_stop;
	/*
	 * 2026-05-30 Round 2.4 retry: "no PM access window" counters.
	 * pm_runtime_get_if_in_use returns 1 ONLY when status==ACTIVE AND
	 * usage_count > 0 (i.e. it's currently safe and meaningful to
	 * touch MMIO). Returns 0 if device is autosuspended, suspending,
	 * or just-idle-with-no-other-holders. So suspended_at_X actually
	 * means "no live MMIO-read window when this boundary was hit"
	 * — which is itself useful: HW that's autosuspended can't be
	 * mid-DMA, ruling out one hypothesis per boundary visit.
	 */
	u32 accum_suspended_at_start;
	u32 accum_suspended_at_stop_out;
	u32 accum_suspended_at_vp9_stop;

	/*
	 * 2026-06-01 R23 — RK3576 HW warmup buffer (port of BSP
	 * rk3576_workaround_*). 8 KiB DMA-coherent buffer populated at probe
	 * with a pre-canned test descriptor, kicked once to establish IP-
	 * internal state required for tile-content VP9 decode. NULL on
	 * variants that don't apply the workaround (only VDPU383 / RK3576).
	 * Freed in rkvdec_remove.
	 */
	void       *rk3576_warmup_cpu;
	dma_addr_t  rk3576_warmup_dma;

	/*
	 * 2026-06-08 Variant B: IRQ-driven warmup (vs the busy-poll in
	 * rkvdec_rk3576_warmup_run). The warmup arms the link IRQ and waits on
	 * warmup_irq_done; a guarded branch at the top of vdpu383_irq_handler
	 * reaps it. warmup_irq_inflight scopes that branch to the warmup window
	 * so it never touches the decode reap. Gated by the warmup_irq param.
	 */
	bool warmup_irq_inflight;
	struct completion warmup_irq_done;

	/*
	 * R35 (2026-06-01) — warmup-on-resume counters. Read from telem
	 * print so we can confirm whether the BSP-parity warmup actually
	 * fires on the runtime_resume path during a decode session.
	 */
	u32 r35_resume_warmups;
	u32 r35_resume_warmups_ok;
	u32 r35_resume_warmups_eio;
	u32 r35_resume_warmups_err;
	u32 r35_resume_total_calls;

	/* R36 (2026-06-01) — warmup-on-start-streaming counters. */
	u32 r36_start_warmups;
	u32 r36_start_warmups_ok;
	u32 r36_start_warmups_eio;
	u32 r36_start_warmups_err;
};

/*
 * Bug-A dmaengine workaround. See ISSUE_1_DMAENGINE_DESIGN_2026-05-24.md.
 * Owned by rkvdec.c (init in probe, release in remove). Per-codec backends
 * use rkvdec_bug_a_copy() from run() to memcpy alt-ref pixels into the
 * current dst buffer when a tiny show-alt-ref frame is detected.
 */
extern int vp9_bug_a_phase;
int rkvdec_bug_a_copy(struct rkvdec_dev *rkvdec,
		      dma_addr_t dst_iova, dma_addr_t src_iova, size_t len);

struct rkvdec_link_table;

#define RKVDEC_LINK_INFLIGHT_MAX 16
/*
 * Phase 3 v0.3 step 2.6: target depth for batched pre-fill in
 * device_run. vp9_run packs descriptors for up to this many m2m
 * buffer pairs before kicking once. Depth=2 is the conservative
 * first step; raise once stable. Must be <= RKVDEC_LINK_INFLIGHT_MAX-1.
 *
 * 2026-05-31 Round 12 v3: dropped from 8 to 1 to test hypothesis
 * that the silent_no_wb 89-93% domination on LM clusters is
 * caused by HW giving up mid-batch when pipelining 8 tasks at
 * once. MPP's model is one-task-per-kick fed continuously by the
 * worker, not our prefill-N-then-kick. Closer alignment to MPP's
 * semantics may reduce HW giveup rate.
 */
/* 2026-05-31 R21 result: BATCH_TARGET=8 tested on YouTube 1080p tile
 * (R3 reject lifted, R19 BSP-style fill in place). Telem: 509 attempts,
 * 15 writebacks (2.9%) — marginally worse than R19's 3.2% with batch=1.
 * Deeper batching does NOT unblock tile decode. Revert to 1 for
 * stability on non-tile (Round 12 v3 setting). */
#define RKVDEC_LINK_BATCH_TARGET 1

struct rkvdec_link_inflight {
	struct vb2_v4l2_buffer *src;
	struct vb2_v4l2_buffer *dst;
	/* 2026-05-28: descriptor slot index this entry was kicked into.
	 * The IRQ handler reads task_status from this slot (HW writes
	 * it on completion); without per-entry slot tracking, batched
	 * fill that rotates through slots 0..15 has no way to find the
	 * right slot to check. */
	u32 slot;
};

struct rkvdec_ctx {
	struct v4l2_fh fh;
	struct v4l2_format coded_fmt;
	struct v4l2_format decoded_fmt;
	const struct rkvdec_coded_fmt_desc *coded_fmt_desc;
	struct v4l2_ctrl_handler ctrl_hdl;
	struct rkvdec_dev *dev;
	enum rkvdec_image_fmt image_fmt;
	struct rkvdec_rcb_config *rcb_config;
	u32 colmv_offset;
	/*
	 * FBC (fbc_enable=1, H.264 8-bit NV12 only for now): the payload-region
	 * offset within the CAPTURE buffer and the reg068 FBC header-stride value.
	 * Computed once in rkvdec_fill_decoded_pixfmt() (single source of truth),
	 * consumed by the codec config_registers(). Both 0 when FBC is disabled,
	 * so the linear path is byte-identical to before.
	 */
	u32 fbc_pld_offset;
	u32 fbc_head_stride;
	void *priv;
	/* Phase 2 LINK mode (rkvdec_link_mode=1). Codec start() sets this
	 * non-NULL when it has allocated a descriptor table; stop() clears
	 * it. vdpu383_irq_handler reads per-task status from the table when
	 * non-NULL. Generic so it doesn't require codec-specific knowledge
	 * in the IRQ path. */
	struct rkvdec_link_table *link_table;
	/* Phase 3 inflight ring: codec device_run detaches src/dst from m2m,
	 * stores them here, then IRQ handler pops oldest entry and calls
	 * v4l2_m2m_buf_done. Lets multiple tasks be in flight while m2m's
	 * device_run keeps cycling. */
	struct rkvdec_link_inflight inflight[RKVDEC_LINK_INFLIGHT_MAX];
	u32 inflight_head;
	u32 inflight_tail;
	spinlock_t inflight_lock;
	/* Phase 3 v0.3 step 2.6: dispatcher (rkvdec_irq_handler) stores
	 * the LINK_DEC_NUM delta here before falling through to the
	 * codec irq_handler. The codec irq drains this many inflight
	 * tasks per fire (could be 1 if HW IRQs per task, or N if HW
	 * IRQs once for a whole batch). */
	u32 last_irq_completed;
	/* Saved by vp9_run before scheduling the watchdog so an
	 * intermediate IRQ (drained part of a batch, more tasks still
	 * in flight) can reschedule the watchdog with the same
	 * threshold. */
	u32 last_watchdog_threshold;
	/* Step A v2 (2026-05-28): silent-completion fast detection.
	 * Reset to 0 by vp9_run; incremented by the watchdog handler
	 * when HW is still mid-decode at the poll interval. When it
	 * reaches RKVDEC_MAX_WATCHDOG_POLLS we stop polling and let
	 * the full watchdog timeout path run. */
	u32 watchdog_poll_count;
	/* BSP-model port: count of quiesced reset+resend retries for the
	 * current stuck task; reset to 0 on any successful reap (progress).
	 * Bounds the reset loop. */
	u32 link_resets;
	u8 has_sps_st_rps: 1;
	u8 has_sps_lt_rps: 1;
};

/* Step A v2: silent-completion polling.
 *
 * Telemetry on BBB200 link mode showed ~260/260 tasks silently
 * complete - HW finishes, clears DEC_ENABLE, never raises STA_INT,
 * no IRQ. Each task waits ~60 ms average for the 80 ms watchdog
 * to notice. That's the entire 15.9 s throughput floor.
 *
 * Solution: watchdog handler polls dec_en at 10 ms intervals.
 *   - HW still running (dec_en==1) -> reschedule at +10 ms, bail
 *     without touching ANY HW state.
 *   - HW finished (dec_en==0) -> fall through to existing silent-
 *     completion path (set state=DONE, soft-reset, job_finish).
 *
 * Bounded at RKVDEC_MAX_WATCHDOG_POLLS so genuinely stuck HW still
 * gets the full timeout handling. 8 * 10 ms = 80 ms total, same
 * as the original watchdog ceiling.
 */
#define RKVDEC_WATCHDOG_POLL_MS    10
#define RKVDEC_MAX_WATCHDOG_POLLS  8
/* BSP-model port: max quiesced reset+resend retries per stuck task. */
#define RKVDEC_MAX_LINK_RESETS     8

static inline struct rkvdec_ctx *file_to_rkvdec_ctx(struct file *filp)
{
	return container_of(file_to_v4l2_fh(filp), struct rkvdec_ctx, fh);
}

/*
 * Inflight ring helpers (Phase 3 v0.3 step 2.2). The ring decouples
 * "task kicked to HW" from "m2m's idea of the current job":
 *
 *   device_run -> fill descriptor + push (src,dst) to inflight + kick
 *   IRQ        -> pop oldest (src,dst) + buf_done
 *
 * At depth=1 the ring is just a pair of pointers carried across the
 * device_run/IRQ boundary, used in place of ctx->run.bufs.  At depth>1
 * (step 2.6) device_run can pre-fill multiple descriptors before
 * kicking, and the IRQ handler reaps them in order.
 *
 * Caller is responsible for ctx->inflight_lock init (codec start()).
 */
static inline int rkvdec_inflight_push(struct rkvdec_ctx *ctx,
				       struct vb2_v4l2_buffer *src,
				       struct vb2_v4l2_buffer *dst,
				       u32 slot)
{
	unsigned long flags;
	u32 head_next;
	int ret = 0;

	spin_lock_irqsave(&ctx->inflight_lock, flags);
	head_next = (ctx->inflight_head + 1) % RKVDEC_LINK_INFLIGHT_MAX;
	if (head_next == ctx->inflight_tail) {
		ret = -ENOSPC;
	} else {
		ctx->inflight[ctx->inflight_head].src = src;
		ctx->inflight[ctx->inflight_head].dst = dst;
		ctx->inflight[ctx->inflight_head].slot = slot;
		ctx->inflight_head = head_next;
	}
	spin_unlock_irqrestore(&ctx->inflight_lock, flags);
	return ret;
}

/* Peek the tail's slot without popping. Returns -1 if ring empty.
 * Used by the IRQ handler to read task_status from the right
 * descriptor slot without removing the inflight entry until we've
 * confirmed the task is actually done. */
static inline int rkvdec_inflight_peek_tail_slot(struct rkvdec_ctx *ctx)
{
	unsigned long flags;
	int ret = -1;

	spin_lock_irqsave(&ctx->inflight_lock, flags);
	if (ctx->inflight_tail != ctx->inflight_head)
		ret = (int)ctx->inflight[ctx->inflight_tail].slot;
	spin_unlock_irqrestore(&ctx->inflight_lock, flags);
	return ret;
}

static inline struct rkvdec_link_inflight
rkvdec_inflight_pop(struct rkvdec_ctx *ctx)
{
	struct rkvdec_link_inflight e = { NULL, NULL };
	unsigned long flags;

	spin_lock_irqsave(&ctx->inflight_lock, flags);
	if (ctx->inflight_tail != ctx->inflight_head) {
		e = ctx->inflight[ctx->inflight_tail];
		ctx->inflight_tail =
			(ctx->inflight_tail + 1) % RKVDEC_LINK_INFLIGHT_MAX;
	}
	spin_unlock_irqrestore(&ctx->inflight_lock, flags);
	return e;
}

static inline u32 rkvdec_inflight_depth(struct rkvdec_ctx *ctx)
{
	unsigned long flags;
	u32 head, tail;

	spin_lock_irqsave(&ctx->inflight_lock, flags);
	head = ctx->inflight_head;
	tail = ctx->inflight_tail;
	spin_unlock_irqrestore(&ctx->inflight_lock, flags);
	return (head - tail + RKVDEC_LINK_INFLIGHT_MAX) %
	       RKVDEC_LINK_INFLIGHT_MAX;
}

enum rkvdec_alloc_type {
	RKVDEC_ALLOC_DMA  = 0,
	RKVDEC_ALLOC_SRAM = 1,
};

struct rkvdec_aux_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
	enum rkvdec_alloc_type type;
};

void rkvdec_run_preamble(struct rkvdec_ctx *ctx, struct rkvdec_run *run);
void rkvdec_run_postamble(struct rkvdec_ctx *ctx, struct rkvdec_run *run);
void rkvdec_memcpy_toio(void __iomem *dst, void *src, size_t len);
void rkvdec_schedule_watchdog(struct rkvdec_dev *rkvdec, u32 timeout_threshold);
void rkvdec_schedule_watchdog_poll(struct rkvdec_dev *rkvdec);

void rkvdec_quirks_disable_qos(struct rkvdec_ctx *ctx);

/* Generic V4L2 ctrl_ops — try_ctrl + s_ctrl that invoke the codec
 * backend's try_ctrl / get_image_fmt callbacks. Codec backends in
 * separate compilation units set this in their ctrl_desc table to opt
 * into bit-depth-driven image_fmt updates (HEVC SPS, VP9 FRAME, etc.). */
extern const struct v4l2_ctrl_ops rkvdec_ctrl_ops;

/* RKVDEC ops */
extern const struct rkvdec_coded_fmt_ops rkvdec_h264_fmt_ops;
extern const struct rkvdec_coded_fmt_ops rkvdec_hevc_fmt_ops;
extern const struct rkvdec_coded_fmt_ops rkvdec_vp9_fmt_ops;

/* VDPU381 ops */
extern const struct rkvdec_coded_fmt_ops rkvdec_vdpu381_h264_fmt_ops;
extern const struct rkvdec_coded_fmt_ops rkvdec_vdpu381_hevc_fmt_ops;

/* VDPU383 ops */
extern const struct rkvdec_coded_fmt_ops rkvdec_vdpu383_h264_fmt_ops;
extern const struct rkvdec_coded_fmt_ops rkvdec_vdpu383_hevc_fmt_ops;
extern const struct rkvdec_coded_fmt_ops rkvdec_vdpu383_vp9_fmt_ops;
extern const struct rkvdec_coded_fmt_ops rkvdec_vdpu383_av1_fmt_ops;

#endif /* RKVDEC_H_ */
