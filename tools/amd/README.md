# Lichii stutter diagnostics

One command records what Lichii is doing while you play, and writes a zip to send back.

## Requirements

- The Linux machine with the RX 9070 XT.
- `python3` (only the standard library is used).
- A Lichii binary or AppImage — yours, or the one built for this project.

`vulkaninfo`, `lspci` and `zip` are used when present; the script carries on without them.

## Run it

```sh
chmod +x lichii-diag.sh frametime_probe.py
./lichii-diag.sh --binary /path/to/lichii
```

If you run an AppImage, point `--binary` at it and the script works out the rest (it falls
back to extract-and-run when `/dev/fuse` is unavailable).

Extra emulator switches can be added for one run without editing anything:

```sh
# e.g. the split-chain branch's own wait counters
./lichii-diag.sh --binary ./lichii --switch EDEN_NX2_ACQUIRE_STATS=1
```

Then, in Lichii:

1. Start Ocarina of Time.
2. Walk through an area that stutters — Hyrule Field, Castle Town, Kokiri Forest.
3. **Press Enter in the terminal** whenever you see a stutter. Each press is timestamped.
4. Close Lichii normally. The bundle is written next to the script:

```
lichii-diag-<timestamp>.zip
```

Send that file back.

If you would rather not have the script launch the emulator, start Lichii yourself with:

```sh
EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545 \
EDEN_NX2_GPU_PROFILE=1 \
EDEN_NX2_PIPELINE_BUILD_TRACE=/tmp/pipeline_build.log \
./lichii
```

and run `frametime_probe.py --out /tmp/frametimes.jsonl` in another terminal.

## What it records

| File | Contents |
|---|---|
| `frametimes.jsonl` | One record per second: `frame_times` (mean/p50/p90/p99/min/max, stutter count, vsync buckets, last 120 frame intervals) plus `status` (title, fps, renderer). Bookmark records where you pressed Enter. |
| `gpu.csv` | Per second: GPU busy %, shader clock, memory clock, board power. Shows whether stutter lines up with a clock or power drop. |
| `pipeline_build.log` | `BEGIN`/`END` per driver pipeline build with wall time in ms. The last `BEGIN` without an `END` is a shader the driver was stuck on. |
| `eden_log.txt` | The normal Lichii log, including per-10 s GPU-thread zone timing from `EDEN_NX2_GPU_PROFILE=1`. |
| `sysinfo/` | Kernel, distro, CPU, GPU, Mesa/Vulkan package versions, `vulkaninfo --summary`. |
| `shader-cache.txt` | Size of the Lichii and Mesa shader caches, so a cold cache can be told from a warm one. |
| `switches.txt` | The `EDEN_*` environment the run actually used. |

## Comparing two runs

When two builds are being A/B tested, one command turns the pair into a verdict:

```sh
python3 compare_bundles.py run-baseline run-candidate
```

It prints each metric for both runs with the relative change, then a summary line
(`N improved, M worse, K unchanged`). Metrics where lower is better — frame time p99, stutters,
blocked acquires, slow pipeline builds — are marked, and only real movement (≥0.5 %) counts;
a metric that did not change is reported as unchanged rather than as a regression.

## Reading it (what we are looking for)

- **`vsync_buckets` concentrated at 1/2/3** with `stutters` high → pacing or vsync problem,
  not raw performance.
- **`p99_ms` several times `p50_ms`, spiky `intervals_ms`** → hitches: pipeline compilation,
  or a per-frame sync stall.
- **Steady but low `fps`, no spikes** → CPU or GPU bound throughput, not stutter.
- **`pipeline_build.log` `END` lines in the tens of ms during play** → compilation stutter;
  the fix is precompilation or cached pipeline binaries.
- **`gpu.csv` shader clock dropping while `fps` drops** → power management. Check
  `power_dpm_force_performance_level` and the GPU's power limit before blaming Lichii.
- **GPU-thread zones dominated by one entry** in `eden_log.txt` → narrows it to draws,
  submits, texture uploads, pipeline lookup or split drain.

## Privacy

No game content, keys, firmware or saves are read. Shader caches are measured by size only.
`.rdc` RenderDoc captures are never collected — those can embed game assets.
