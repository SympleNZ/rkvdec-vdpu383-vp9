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

Current baseline: **107/305** (`docs/fluster_baseline.csv`). See
`docs/CONFORMANCE.md` for the per-cluster breakdown — the misses are dominated by
the reference-bypass bug (`docs/REF_BYPASS_BUG.md`) and downstream gst/comparator
issues, not core decode. **Reboot before a fresh baseline** (state bleeds across runs).

## Quick visual check

```sh
gst-launch-1.0 filesrc location=clip.webm ! matroskademux ! vp9parse ! \
    v4l2slvp9dec ! kmssink force-modesetting=true
```

A large-frame, well-coded clip should display perfectly; a clip full of small
prediction-heavy inter frames (most real streaming content — alt-ref overlays,
skip-heavy P-frames) will show corruption — that is the open bug, not a setup
problem. See `docs/REF_BYPASS_BUG.md`.
