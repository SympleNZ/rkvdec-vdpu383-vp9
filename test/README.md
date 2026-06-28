# Tests

## Smoke (build + load + format check)

```sh
sudo ./test/build_and_smoke.sh
```

Builds the module, reloads it, and confirms the VP9 stateless format (`VP9F`)
registers on `/dev/video-dec0`.

## Conformance

Use [Fluster](https://github.com/fluendo/fluster) — it compares against the
libvpx reference with strict MD5:

```sh
python3 fluster.py run -d GStreamer-VP9-V4L2SL -ts VP9-TEST-VECTORS -j1
```

Current baseline: **148/305** strict-MD5 (`docs/fluster_baseline.csv`). See
`docs/CONFORMANCE.md` for the per-cluster breakdown — core decode passes 94–100 %;
the misses are dominated by small-dimension padding / resize stressors and the
**VP9 compound-prediction gap**, not by core-decode failures. **Reboot before a
fresh baseline** (decoder state bleeds across runs).

## Quick visual check

```sh
gst-launch-1.0 filesrc location=clip.webm ! matroskademux ! vp9parse ! \
    v4l2slvp9dec ! kmssink force-modesetting=true
```

A KEY / single-reference / low-motion clip (most large-frame, well-coded content)
displays **bit-exact / perfect**. **Compound / high-motion** content
(`reference_mode = SELECT` — alt-ref overlays and much real-world streaming VP9)
shows corruption on a content-specific subset of frames — that is **the open
compound-prediction gap, below the V4L2 interface** (not a setup problem, and not
driver-addressable). Full triage in the main [`README.md`](../README.md)
("VP9 compound prediction"); [`docs/REF_BYPASS_BUG.md`](../docs/REF_BYPASS_BUG.md)
is retained as investigation history (its "reference-bypass" framing is superseded
by the compound-MC conclusion).
