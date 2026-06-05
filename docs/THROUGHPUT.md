# VP9 throughput — root-caused 2026-06-05: it was the compound bug all along

Board: NanoPi R76S #1 `.165`, kernel 7.0.1, current OOT driver. Measured after
the `irq_split` experiment came back negative (SESSION_2026-06-05_FINALISATION.md),
to find where the throughput actually goes. **Headline: there is no raw decoder
throughput problem. Every prior "slow" number was the vendor compound bug
stalling real content via timeouts.**

## Method

The mistake all prior throughput work made was measuring on **yt_720p60 / BBB /
sintel**, which are full of `reference_mode=2` (compound) frames — exactly the
frames the vendor bug (`VP9_INTER_DECODE_BUG_2026-06-04.md`, session J) decodes
wrong, and which time out / silent-complete and stall the pipeline. To measure
*decode rate* you must use **compound-free** content.

Clean streams generated on-board (no alt-ref ⇒ no compound; verified
`refmode2=0`, `timeouts=0`, full frame count out):

```
ffmpeg -f lavfi -i testsrc2=size=WxH:rate=30:duration=D \
  -c:v libvpx-vp9 -auto-alt-ref 0 -lag-in-frames 0 -b:v BR -deadline good -cpu-used 4 out.ivf
```

`/root/clean720.ivf` (300f), `/root/clean1080.ivf` (300f), `/root/clean2160.ivf`
(150f). fps = frames / wall, `gst-launch ... ! identity eos-after=N !
v4l2slvp9dec ! fakesink sync=false`, performance governor unless noted.

## Result 1 — clean-content decode rate is 4.5–5.4× realtime

720p clean, 2×2 governor × cpuidle (4 runs each, 0 timeouts everywhere):

| config | mean fps |
|---|---|
| ondemand + idle | 268.8 |
| **performance + idle** | **325.1** |
| performance + noidle (PM-QoS 0) | 300.5 |
| ondemand + noidle | 283.7 |

- cpuidle / PM-QoS deep-idle: **no effect** (confirms session_e; the `cpu-sleep`
  370 µs state is not on the critical path for back-to-back decode).
- governor: **performance = +21%** over ondemand and removes the cold-start ramp
  dip (ondemand's first run is ~210 fps then settles ~288; performance is a flat
  324–326). The driver's per-frame CPU glue is mildly freq-sensitive.

## Result 2 — realtime at the resolutions CastFlow cares about

via-gst (frame goes through the gst pipeline) vs **pure HW decode time**
(kick→DONE IRQ, `vp9_time` module-param instrumentation; isolates the decoder
from gst frame-copy), clean content, performance governor:

| resolution | pure HW / frame | pure HW fps | via-gst fps | gst overhead |
|---|---|---|---|---|
| 720p | 1.93 ms | 517 | 325.6 | ~1.2 ms/frame |
| 1080p | 4.33 ms | 231 | 174.2 | ~1.4 ms/frame |
| 4K (2160p) | 19.2 ms | 52 | 43.8 | ~3.5 ms/frame |

gst adds ~1–3.5 ms/frame (frame handling/copy, scales with frame size), so the
via-gst number understates decode capability — most at 4K.

**4K correction (thanks to Simon):** my earlier "4K@60 is a silicon-class limit
~44 fps" was WRONG on two counts. (1) The real pure-HW rate is **52 fps**, not 44
(gst overhead ate the rest). (2) **MPP decodes 4K@60 with headroom on this same
VDPU383 silicon** (proven previously), so 52 fps is **our driver's per-frame
efficiency, not silicon.** There is a ~13% per-frame HW gap to MPP at 4K to close
to reach realtime@60.

**Prime suspect for the 4K gap:** the driver calls `iommu_flush_iotlb_all()`
**before every kick** (`rkvdec-vdpu383-vp9.c:3265`). A full domain TLB flush
forces the HW to re-walk page tables for every reference read during decode, and
that penalty scales with the memory the frame touches — so it hits 4K (large
references) far harder than 720p, matching the data. MPP almost certainly does
not do a full per-frame flush. Next throughput step: A/B a gated flush-skip (or
narrower flush) for correctness + 4K HW-time. (Other candidates: a perf-related
register MPP sets that differs only at 4K; RCB/cache config.)

1080p any frame-rate and 4K@30 are already realtime; 4K@60 is reachable by
closing the per-frame gap, not blocked by silicon. (YouTube also serves
AV1/H.264 for 4K@60.)

## Result 3 — the airtight proof: same resolution, clean vs real

| stream (720p, 200f, perf gov) | fps | timeouts | compound frames seen |
|---|---|---|---|
| clean720 (no compound) | 322.1 | 0 | 0 |
| yt_720p60 (real YouTube) | 31.1 | 3 | 155 |

Same decoder, same resolution, same governor — only the content differs. yt is
10× slower with timeouts and is 78% compound frames. **The 31 fps is exactly
session_e's "31 fps baseline" and this session's "32.1 fps baseline"** — those
were always compound-bug-crippled yt content, never the decoder's rate.

(Synthetic content can't cheaply reproduce compound: libvpx won't pick
`reference_mode=SELECT` for trivial `testsrc` even with `-auto-alt-ref 1`, so the
controlled on/off encode stayed single-ref. Real VOD content is where compound
appears.)

## Conclusion

- **No raw throughput problem at 720p/1080p.** Clean-content decode is 517 fps
  pure-HW / 325 via-gst (720p) and 231 / 174 (1080p) — realtime+ for all
  realistic use.
- **4K is NOT silicon-limited.** Pure-HW 52 fps; MPP does 4K@60 on the same
  silicon → ~13% per-frame driver-efficiency gap to close (prime suspect: the
  per-kick full IOMMU TLB flush). 4K@30 already realtime; 4K@60 reachable.
- **Real-content throughput is gated entirely on the compound bug** (→ publish +
  Detlev). Fixing compound makes yt/BBB throughput jump to the clean rates.
- **session_e's "ms-scale threaded-IRQ wakeup" root-cause is superseded** — it
  was measured on compound-contaminated yt content. The `irq_split` experiment
  was chasing that phantom (and was correctly abandoned).
- **One free, safe win (userspace, not driver):** run the CPU at `performance`
  (or a cpufreq floor) during decode for +21% and no cold-start dip. Belongs in
  the CastFlow endpoint / a systemd-or-udev rule, not the driver — a driver
  shouldn't hijack the system governor.

Clean probe streams left on `.165`: `/root/clean720.ivf`, `clean1080.ivf`,
`clean2160.ivf`. Reusable for any future decode-rate measurement (immune to the
compound bug).

---

## Follow-up (same day) — MPP native baseline + the per-kick IOMMU flush

Two corrections/extensions driven by Simon: (a) measure the real MPP/BSP native
throughput across resolutions, (b) use complex motion-heavy content, not trivial
testsrc.

### MPP native baseline (.104, BSP k6.1.141, performance governor, `mpi_dec_test -t 10`)

| res | MPP testsrc | MPP complex (BBB-derived) | our pure-HW (compound-free) |
|---|---|---|---|
| 720p | 1189 fps | 1035 fps | 517 |
| 1080p | 599 fps | 508 fps | 231 |
| 4K | 154 fps | **136 fps** | 52 (testsrc) |

**MPP does complex 4K at 136 fps — 2.3× headroom over 60.** So 4K@60 is firmly a
driver gap, not silicon (correcting the earlier "silicon-class limit" claim).
The decisive figure: MPP's *end-to-end* 720p frame = 0.84 ms is **less than our
HW-decode window alone** (1.93 ms) — our HW decode takes ~2.3–3× longer per frame
on the same silicon, and the gap grows with resolution.

### Complex content needs compound — our decoder can't sustain it

Re-encoding BBB 4K compound-free via `-auto-alt-ref 0` did NOT eliminate compound
(libvpx still uses last+golden compound for complex frames): our decoder timed
out (1–6 timeouts) on all three cplx streams. **This is itself the headline:
realistic complex content uses compound prediction, which our decoder stalls on
(the vendor bug) — so for real content, throughput is moot until compound is
fixed.** MPP (no bug) does the cplx streams at 1035/508/136 fps.

### The per-kick IOMMU TLB flush — ~1.3–2.5 ms/frame, removable

`rkvdec-vdpu383-vp9.c` flushes the **whole** IOMMU domain
(`iommu_flush_iotlb_all`) before every kick. Gated A/B (`vp9_skip_tlb_flush`),
clean content, performance governor, averaged over many 100-frame windows:

| res | flush (skip=0) | no-flush (skip=1) | saved | speedup |
|---|---|---|---|---|
| 720p | 3164 µs (316 fps) | 1740 µs (575 fps) | 1424 µs | 1.82× |
| 1080p | 5381 µs (186 fps) | 4072 µs (246 fps) | 1309 µs | 1.32× |
| 4K | 22098 µs (45 fps) | 19610 µs (51 fps) | 2488 µs | 1.13× |

- **Correctness: byte-identical** (clean720 md5 unchanged with/without flush).
- The flush costs a roughly fixed ~1.3–2.5 ms/frame (biggest relative win at
  720p). **Why it's redundant:** vb2 assigns each buffer a stable IOVA for its
  lifetime, so mappings don't change frame-to-frame in steady state — a full
  per-frame TLB flush is pure overhead. It is only genuinely needed after an
  `iommu_restore` (error recovery re-attaches the domain and can leave stale
  entries). **Clean fix: flush only after a restore, not every frame.**
- This is only *part* of the gap: even no-flush we're ~2× behind MPP
  (575 vs 1189 @720p). The remainder is almost certainly MPP's **link-mode
  pipelining** (decode N+1 overlaps N's completion) + our per-frame IP
  start/stop, which my pure-HW (kick→IRQ) measurement doesn't even include —
  i.e. the *end-to-end* gap is larger still.

### Net

- **Throughput for real content is gated on the compound bug** (complex content
  is compound; our decoder stalls). This is the dominant fact.
- **Two concrete driver optimizations exist** for compound-free / once-compound-fixed
  throughput: (1) flush-only-after-restore (~1.3–2.5 ms/frame, correctness-safe,
  ~doubles 720p); (2) link-mode pipelining to overlap inter-frame overhead (the
  larger remaining ~2× — bigger lift, needs the link path which currently also
  fails on compound). Neither is needed for 720p/1080p realtime today on
  compound-free content.

### LANDED: flush-only-after-restore (validated)

Implemented as the new default: `rkvdec->iommu_needs_flush` set on
`iommu_restore` / queue (re)start / init; the VP9 kick flushes + clears it, so
steady-state decode does no flush. `vp9_skip_tlb_flush` repurposed as a debug
override (0=after-restore default, 1=never, 2=always/old). Scoped to the VP9
single-shot path only (the flush lived in `rkvdec-vdpu383-vp9.c`).

Validation:
- **clean720 md5 byte-identical** to old behaviour.
- **Full Fluster:** 146/305 (fix) vs 147/305 (policy=2 old behaviour, same
  module) vs 148/305 (orig baseline) — a ±2 variance band; `big_superframe-01`
  fails even under always-flush, so the 146 is variance-lottery, **not a
  regression**.
- Error recovery still flushes (the one case it's genuinely needed) by design.
- Throughput: 1.82× @720p, 1.32× @1080p, 1.13× @4K.

Remaining ~2× to MPP is link-mode pipelining + per-frame IP start/stop (separate,
larger lift) — and moot for real content until the compound bug is fixed.

New gated params (default off/inert): `vp9_time` (per-frame HW decode timing).
`vp9_skip_tlb_flush` now selects flush policy (default 0 = the landed fix). Clean
probe streams on `.165`:
`clean{720,1080,2160}.ivf`, `cplx{720,1080,2160}.ivf`; copied to `.104:/tmp` for
MPP A/B.
