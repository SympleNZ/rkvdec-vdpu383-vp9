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

Current baseline: **148/305** (`docs/fluster_baseline.csv`). See
`docs/CONFORMANCE.md` for the per-cluster breakdown — the misses are dominated by
the compound bug (`docs/COMPOUND_BUG.md`) and downstream gst/comparator issues,
not core decode. **Reboot before a fresh baseline** (state bleeds across runs).

## Quick visual check

```sh
gst-launch-1.0 filesrc location=clip.webm ! matroskademux ! vp9parse ! \
    v4l2slvp9dec ! kmssink force-modesetting=true
```

A compound-free clip should display perfectly; a compound-heavy clip (most real
streaming content) will show corruption — that is the open bug, not a setup
problem. Count compound frames in a clip with the `vp9-run` pr_debug enabled and
`grep -c 'refmode=2'` (see `docs/BUILD_AND_TEST.md`).
