#!/usr/bin/env bash
# Collect a diagnostic bundle for an AMD/RADV stutter report.
#
# Runs Lichii with the live debug server and the GPU-thread profiler on, samples
# frame times continuously while you play, and zips the result. Nothing from the
# game (ROM, keys, firmware, saves) is read or included: only logs, timings,
# counters and system information.
#
#   ./lichii-diag.sh --binary /path/to/lichii [...game args]
#
# Press Enter at any moment to bookmark the current time (do it when you see a
# stutter). Close Lichii normally when you are done; the bundle is written next
# to this script.

set -uo pipefail

BINARY=""
OUTROOT=""
DURATION=0
SELFTEST=0
GAME_ARGS=()
PORT_STR="127.0.0.1:6545"
EXTRA_SWITCHES=()
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  --binary PATH       Lichii binary or AppImage (or set LICHII=/path/to/lichii)
  --outdir DIR        where to write the bundle (default: ./lichii-diag-<time>)
  --duration SECS     stop after SECS instead of waiting for Lichii to exit
  --switch NAME=VAL   extra EDEN_NX2_* switch for this run (repeatable)
  --selftest          start Lichii, wait for the game to report in, then exit
  -h, --help          this text

Everything after `--` is passed to Lichii unchanged.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary) BINARY="${2:-}"; shift 2 ;;
        --outdir) OUTROOT="${2:-}"; shift 2 ;;
        --duration) DURATION="${2:-0}"; shift 2 ;;
        --switch) EXTRA_SWITCHES+=("${2:-}"); shift 2 ;;
        --selftest) SELFTEST=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; GAME_ARGS=("$@"); break ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

log() { printf '\033[1m==>\033[0m %s\n' "$*"; }

# Lichii is started in its own session so stopping it also stops anything it
# spawned; otherwise a leftover child can keep the debug port and the next
# run silently measures the old process.
stop_lichii() {
    [[ -z "${LICHII_PID:-}" ]] && return 0
    kill -TERM "-$LICHII_PID" 2>/dev/null || kill -TERM "$LICHII_PID" 2>/dev/null
    for _ in 1 2 3 4 5; do
        kill -0 "$LICHII_PID" 2>/dev/null || return 0
        sleep 1
    done
    kill -KILL "-$LICHII_PID" 2>/dev/null || kill -KILL "$LICHII_PID" 2>/dev/null
}

if [[ -z "$BINARY" ]]; then
    BINARY="${LICHII:-}"
fi

# A path pasted from a wrapped command often carries a trailing space or CR.
if [[ -n "$BINARY" ]]; then
    BINARY="$(printf '%s' "$BINARY" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's/\r$//')"
fi
[[ "$BINARY" == "~/"* ]] && BINARY="$HOME/${BINARY#\~/}"

if [[ -z "$BINARY" ]]; then
    if command -v lichii >/dev/null 2>&1; then
        BINARY="$(command -v lichii)"
    else
        # A Lichii AppImage or binary sitting in the current directory.
        BINARY="$(ls -1 ./*.AppImage ./*.appimage ./lichii 2>/dev/null | head -n 1)"
    fi
fi

if [[ -z "$BINARY" ]]; then
    echo "error: could not find Lichii. Pass --binary PATH." >&2
    echo "       Looked for lichii in \$PATH and for *.AppImage in $PWD" >&2
    exit 2
fi

if [[ ! -e "$BINARY" ]]; then
    echo "error: no such file: $BINARY" >&2
    echo "       Run this from the directory that holds the build, or pass its full path." >&2
    exit 2
fi

# A browser download usually drops the executable bit.
if [[ -f "$BINARY" && ! -x "$BINARY" ]]; then
    if chmod +x "$BINARY" 2>/dev/null; then
        log "added the executable bit to $BINARY"
    else
        echo "error: $BINARY is not executable and cannot be made so." >&2
        echo "       Run: chmod +x '$BINARY'" >&2
        exit 2
    fi
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
OUTDIR="${OUTROOT:-$PWD/lichii-diag-$STAMP}"
mkdir -p "$OUTDIR" || { echo "error: cannot create $OUTDIR" >&2; exit 2; }
OUTDIR="$(cd "$OUTDIR" && pwd)"
mkdir -p "$OUTDIR/sysinfo"

PROBE_PID=""
GPU_MON_PID=""
CPU_MON_PID=""
LICHII_PID=""

cleanup() {
    [[ -n "$PROBE_PID" ]] && kill "$PROBE_PID" 2>/dev/null
    [[ -n "$GPU_MON_PID" ]] && kill "$GPU_MON_PID" 2>/dev/null
[[ -n "$CPU_MON_PID" ]] && kill "$CPU_MON_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

# The driver pipeline cache only grows when a pipeline had to be built, so a
# before/after snapshot separates compilation stutter from plain load.
snapshot_shader_cache() {
    local out="$1"
    : > "$out"
    # XDG_CACHE_HOME/eden/shader/<title id>/vulkan*.bin is where nx2 puts them;
    # the data directory is a different place and held nothing.
    for base in "${XDG_CACHE_HOME:-$HOME/.cache}/eden/shader" \
                "$HOME/.cache/eden/shader"; do
        [[ -d "$base" ]] || continue
        find "$base" -maxdepth 3 -type f -name 'vulkan*' -printf '%s %p\n' 2>/dev/null >> "$out"
    done
    sort -k2 -u -o "$out" "$out" 2>/dev/null || true
}

# ---------------------------------------------------------------- system info
log "collecting system information"
{
    echo "date: $(date -Is)"
    echo "hostname: $(uname -n)"
    echo "kernel: $(uname -srm)"
    echo "distro: $( . /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-unknown}" )"
    echo "session: ${XDG_SESSION_TYPE:-unknown} / ${XDG_CURRENT_DESKTOP:-unknown}"
    echo "binary: $BINARY"
} > "$OUTDIR/sysinfo/summary.txt" 2>&1

lscpu > "$OUTDIR/sysinfo/lscpu.txt" 2>&1
if command -v lspci >/dev/null 2>&1; then
    lspci -nnk > "$OUTDIR/sysinfo/lspci.txt" 2>&1
fi
free -h > "$OUTDIR/sysinfo/memory.txt" 2>&1

# Mesa / Vulkan driver version, whichever package manager this distro has.
{
    if command -v pacman >/dev/null 2>&1; then
        pacman -Q mesa vulkan-radeon lib32-mesa 2>/dev/null
        pacman -Q | grep -Ei 'mesa|vulkan|amdvlk|amdgpu' 2>/dev/null
    fi
    if command -v rpm >/dev/null 2>&1; then
        rpm -qa 2>/dev/null | grep -Ei 'mesa|vulkan|amdvlk' | sort
    fi
    if command -v dpkg >/dev/null 2>&1; then
        dpkg -l 2>/dev/null | grep -Ei 'mesa|vulkan|amdvlk' | awk '{print $2, $3}'
    fi
} > "$OUTDIR/sysinfo/packages.txt" 2>&1

if command -v vulkaninfo >/dev/null 2>&1; then
    vulkaninfo --summary > "$OUTDIR/sysinfo/vulkaninfo.txt" 2>&1
    # The read-only table-pointer binding is clamped to maxStorageBufferRange.
    # If the driver reports a small value, Ocarina's light probes (which sit
    # around 130 MiB past their pointer) fall outside the binding again.
    vulkaninfo 2>/dev/null | grep -iE \
        "maxStorageBufferRange|maxMemoryAllocationSize|maxBufferSize|deviceName|driverInfo" \
        > "$OUTDIR/sysinfo/vulkan-limits.txt" 2>&1
else
    echo "vulkaninfo not installed" > "$OUTDIR/sysinfo/vulkaninfo.txt"
fi

display_switch=""
# Qt picks Wayland or X11 by itself; Lichii has no --enable-qt-wayland flag and
# passes unknown options through to the game.

# ----------------------------------------------------------------- switches
# Keep the profile cheap: the point is frame timing, not per-draw tracing.
export EDEN_NX2_DEBUG_SERVER="$PORT_STR"
export EDEN_NX2_GPU_PROFILE=1
export EDEN_NX2_PIPELINE_BUILD_TRACE="$OUTDIR/pipeline_build.log"
# Once a second, splits each frame's wall time between swapchain acquire,
# command recording and vkQueuePresentKHR. That is the only measurement here
# that says whether the CPU side or the GPU is the limit, and it costs one line
# per second, so it is always on.
export EDEN_DKB_FRAME_TRACE=1

for entry in "${EXTRA_SWITCHES[@]}"; do
    name="${entry%%=*}"
    value="${entry#*=}"
    if [[ "$name" == "$entry" ]]; then
        export "$name=1"
    else
        export "$name=$value"
    fi
done

{
    for name in $(env | grep -oE '^(EDEN_[A-Z0-9_]+)=' | tr -d '='); do
        printf '%s=%s\n' "$name" "$(printenv "$name")"
    done
} > "$OUTDIR/switches.txt" 2>&1

# ---------------------------------------------------------------- shader cache
# A warm shader cache means the stutter is not compilation; record its size.
cache_candidates=(
    "${XDG_CACHE_HOME:-$HOME/.cache}/eden"
    "$HOME/.cache/mesa_shader_cache"
    "${XDG_CACHE_HOME:-$HOME/.cache}/mesa_shader_cache"
)
{
    echo "MESA_SHADER_CACHE_DIR=${MESA_SHADER_CACHE_DIR:-<unset>}"
    echo "MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-<unset>}"
    for entry in "${cache_candidates[@]}"; do
        [[ -d "$entry" ]] && du -sh "$entry" 2>/dev/null
    done
    find "${XDG_CACHE_HOME:-$HOME/.cache}" -maxdepth 4 -name 'vulkan*.bin' \
         -printf '%p %s\n' 2>/dev/null | head -n 50
} > "$OUTDIR/shader-cache.txt" 2>&1

# ------------------------------------------------------------------- launch
snapshot_shader_cache "$OUTDIR/shader-cache-before.txt"

cat <<EOF

Will now start Lichii with:
  EDEN_NX2_DEBUG_SERVER=$PORT_STR
  EDEN_NX2_GPU_PROFILE=1
  EDEN_DKB_FRAME_TRACE=1
  EDEN_NX2_PIPELINE_BUILD_TRACE=$OUTDIR/pipeline_build.log

In the game: walk through an area that stutters, and press Enter here whenever
you see a stutter (each press is timestamped). Then close Lichii normally.

EOF

EXTRA=()
if [[ "$BINARY" == *.AppImage || "$BINARY" == *.appimage ]]; then
    if [[ ! -r /dev/fuse ]]; then
        log "no /dev/fuse; running the AppImage with extract-and-run"
        export APPIMAGE_EXTRACT_AND_RUN=1
    fi
    [[ -n "$display_switch" ]] && EXTRA+=("$display_switch")
fi

log "starting Lichii"
if command -v setsid >/dev/null 2>&1; then
    setsid "$BINARY" "${EXTRA[@]}" "${GAME_ARGS[@]}" \
        > "$OUTDIR/lichii-stdout.log" 2>&1 &
else
    "$BINARY" "${EXTRA[@]}" "${GAME_ARGS[@]}" \
        > "$OUTDIR/lichii-stdout.log" 2>&1 &
fi
LICHII_PID=$!

# ---------------------------------------------------------------- self-test
# Answers "will a real run work?" in under a minute, instead of after a full
# play session that produces an empty bundle.
if [[ "$SELFTEST" -eq 1 ]]; then
    PROBE="$SCRIPT_DIR/frametime_probe.py"
    log "self-test: load the game when Lichii appears (Ctrl+C to stop)"
    reachable=0
    emu_exited=0
    for tick in $(seq 1 150); do
        if ! kill -0 "$LICHII_PID" 2>/dev/null; then
            emu_exited=1
            break
        fi
        if python3 "$PROBE" --server "$PORT_STR" --once > "$OUTDIR/selftest-status.json" 2>/dev/null; then
            reachable=1
            break
        fi
        if (( tick % 15 == 0 )); then
            log "...still waiting for a game to start ($((tick * 2))s)"
        fi
        sleep 2
    done

    if [[ "$reachable" -eq 1 ]]; then
        echo
        log "the debug server answered. This build works for measuring:"
        sed 's/^/    /' "$OUTDIR/selftest-status.json"
        echo
        echo "Now run the real thing:"
        echo "  $0 --binary $BINARY --outdir $OUTDIR"
        stop_lichii
        exit 0
    fi

    echo
    if [[ "$emu_exited" -eq 1 ]]; then
        echo "Lichii exited before its debug server answered." >&2
    else
        echo "The debug server did not answer within 5 minutes." >&2
    fi
    echo "Most likely the game was not started, or this build has no debug server." >&2
    echo "Checked: $(basename "$BINARY") on $PORT_STR" >&2
    stop_lichii
    exit 1
fi

# ------------------------------------------------------------- gpu monitor
# RDNA4 clocks and board power swing with load; a stutter that lines up with a
# clock or power drop is a power-management problem, not an emulator one.
CARD="$(ls -d /sys/class/drm/card*/device 2>/dev/null | head -n 1)"
if [[ -n "$CARD" ]]; then
    (
        echo "t,gpu_busy_percent,sclk_mhz,power_w,mclk_mhz"
        while :; do
            busy="$(cat "$CARD/gpu_busy_percent" 2>/dev/null | tr -d ' \n')"
            sclk="$(cat "$CARD"/hwmon/hwmon*/freq1_input 2>/dev/null | head -n1 | tr -dc '0-9')"
            mclk="$(cat "$CARD"/hwmon/hwmon*/freq2_input 2>/dev/null | head -n1 | tr -dc '0-9')"
            power="$(cat "$CARD"/hwmon/hwmon*/power1_average 2>/dev/null | head -n1 | tr -dc '0-9')"
            printf '%s,%s,%s,%s,%s\n' \
                "$(date +%s.%N)" \
                "${busy:-}" \
                "$(( ${sclk:-0} / 1000000 ))" \
                "$(( ${power:-0} / 1000000 ))" \
                "$(( ${mclk:-0} / 1000000 ))"
            sleep 1
        done
    ) > "$OUTDIR/gpu.csv" 2>/dev/null &
    GPU_MON_PID=$!
fi

# ------------------------------------------------------------- cpu monitor
# Linux has no NCE, so guest code runs on the Dynarmic backend and the emulated
# CPU is a real suspect for an area-dependent frame rate. Per-thread CPU time
# says whether the guest threads or the render thread are the ones saturating.
#
# The AppImage launcher may fork, and the emulator's threads then live in a
# child. Lichii is started with setsid, so its process group is ours to search:
# watch whichever member has the most threads.
if [[ -d /proc/"$LICHII_PID" ]]; then
    (
        echo "t,tid,comm,ticks"
        while :; do
            target=""
            best=0
            for candidate in $(ps -g "$LICHII_PID" -o pid= 2>/dev/null); do
                count=$(ls "/proc/$candidate/task" 2>/dev/null | wc -l)
                if [[ "$count" -gt "$best" ]]; then
                    best="$count"
                    target="$candidate"
                fi
            done
            [[ -z "$target" ]] && break
            now="$(date +%s.%N)"
            for stat in /proc/"$target"/task/*/stat; do
                [[ -r "$stat" ]] || continue
                tid="$(basename "$(dirname "$stat")")"
                comm="$(tr -d '\n' < "/proc/$target/task/$tid/comm" 2>/dev/null | tr ',' ';')"
                # utime and stime are fields 14 and 15; comm may contain spaces,
                # so count from after the last ')'.
                ticks="$(sed 's/.*) //' "$stat" 2>/dev/null | awk '{print $12+$13}')"
                printf '%s,%s,"%s",%s\n' "$now" "$tid" "${comm:-?}" "${ticks:-0}"
            done
            sleep 1
        done
    ) > "$OUTDIR/cpu.csv" 2>/dev/null &
    CPU_MON_PID=$!
fi

# ------------------------------------------------------------------- probe
PROBE="$SCRIPT_DIR/frametime_probe.py"
if [[ ! -f "$PROBE" ]] || ! command -v python3 >/dev/null 2>&1; then
    log "warning: python3 or frametime_probe.py missing; timings will be incomplete"
else
    python3 "$PROBE" --server "$PORT_STR" --out "$OUTDIR/frametimes.jsonl" \
        --interval 1 --window 5 --refresh-hz 60 \
        > "$OUTDIR/probe.log" 2>&1 &
    PROBE_PID=$!
    log "probe running (pid $PROBE_PID)"
fi

# ------------------------------------------------------------------- wait
# With a terminal, block up to a second on Enter so a press bookmarks the
# moment. Without one there is nothing to read, so sleep instead of spinning.
elapsed=0
interactive=0
[[ -t 0 ]] && interactive=1
while kill -0 "$LICHII_PID" 2>/dev/null; do
    if [[ "$DURATION" -gt 0 && "$elapsed" -ge "$DURATION" ]]; then
        log "duration reached; stopping"
        break
    fi
    if [[ "$interactive" -eq 1 ]]; then
        if read -r -t 1 -s _ && [[ -n "$PROBE_PID" ]]; then
            kill -USR1 "$PROBE_PID" 2>/dev/null && log "bookmark"
        fi
    else
        sleep 1
    fi
    elapsed=$((elapsed + 1))
done

log "Lichii exited; collecting logs"
sleep 1
# With --duration the emulator is still running; stop the one we started.
if kill -0 "$LICHII_PID" 2>/dev/null; then
    log "stopping Lichii"
    stop_lichii
fi
[[ -n "$PROBE_PID" ]] && kill "$PROBE_PID" 2>/dev/null
[[ -n "$GPU_MON_PID" ]] && kill "$GPU_MON_PID" 2>/dev/null
[[ -n "$CPU_MON_PID" ]] && kill "$CPU_MON_PID" 2>/dev/null
PROBE_PID=""
GPU_MON_PID=""
CPU_MON_PID=""

# --------------------------------------------------------------- logs
log_dirs=(
    "${XDG_DATA_HOME:-$HOME/.local/share}/eden/log"
    "$HOME/.local/share/eden/log"
    "$PWD/user/log"
)
for dir in "${log_dirs[@]}"; do
    if [[ -f "$dir/eden_log.txt" ]]; then
        cp -f "$dir/eden_log.txt" "$OUTDIR/eden_log.txt" 2>/dev/null
        [[ -f "$dir/eden_log.txt.old" ]] && \
            cp -f "$dir/eden_log.txt.old" "$OUTDIR/eden_log.txt.old" 2>/dev/null
        echo "$dir" > "$OUTDIR/log-source.txt"
        break
    fi
done

# --------------------------------------------------------------- summary
# Identify the run from the emulator's own log, so a bundle says which build
# produced it without trusting the filename.
if [[ -f "$OUTDIR/eden_log.txt" ]]; then
    # Strip the "[ time] Category <Level>" prefix the log puts in front of every line.
    log_line() {
        local pattern="$1" fallback="$2" mode="${3:-first}" found
        if [[ "$mode" == "last" ]]; then
            found="$(grep -E "$pattern" "$OUTDIR/eden_log.txt" 2>/dev/null | tail -n 1)"
        else
            found="$(grep -m1 -E "$pattern" "$OUTDIR/eden_log.txt" 2>/dev/null | head -n 1)"
        fi
        [[ -z "$found" ]] && found="$fallback"
        printf '%s\n' "$found" | sed -E 's/^\[[^]]*\] [A-Za-z0-9_]+ <[A-Za-z]+> //'
    }
    {
        log_line 'Lichii Version:' 'Lichii Version: (not in log)'
        log_line 'Booting game:' 'Booting game: (no boot line in log)' last
        log_line 'Debug server listening' 'Debug server: (no listening line)'
    } > "$OUTDIR/run-info.txt" 2>&1
else
    echo "eden_log.txt was not found; the run cannot be identified" > "$OUTDIR/run-info.txt"
fi

SAMPLES="$(wc -l < "$OUTDIR/frametimes.jsonl" 2>/dev/null || echo 0)"
SAMPLES="${SAMPLES//[[:space:]]/}"

snapshot_shader_cache "$OUTDIR/shader-cache-after.txt"
awk '
    NR == FNR { before[$2] = $1; next }
    { after[$2] = $1 }
    END {
        for (file in after) {
            if (!(file in before)) { new_files++; new_bytes += after[file] }
            else if (after[file] > before[file]) { grew++; grew_bytes += after[file] - before[file] }
        }
        printf "new cache files during the run: %d (%d bytes)\n", new_files, new_bytes
        printf "existing cache files that grew: %d (%d bytes)\n", grew, grew_bytes
        if (new_files + grew > 0)
            print "pipelines were built while playing: compilation stutter is present"
        else
            print "no cache growth: nothing compiled while playing"
    }
' "$OUTDIR/shader-cache-before.txt" "$OUTDIR/shader-cache-after.txt" \
    > "$OUTDIR/shader-cache-delta.txt" 2>/dev/null || true

{
    echo "bundle: $OUTDIR"
    echo "samples: $SAMPLES"
    echo "marks: $(grep -c '"mark": true' "$OUTDIR/frametimes.jsonl" 2>/dev/null || true)"
    [[ -f "$OUTDIR/run-info.txt" ]] && cat "$OUTDIR/run-info.txt"
    [[ -f "$OUTDIR/shader-cache-delta.txt" ]] && cat "$OUTDIR/shader-cache-delta.txt"
} > "$OUTDIR/SUMMARY.txt" 2>&1

# A bundle with no samples cannot be analysed; say what to check rather than
# handing over an empty zip.
if [[ "${SAMPLES:-0}" -lt 1 ]]; then
    cat <<'EOF' >&2

!! No frame-time samples were recorded.

The emulator ran, but its debug server never answered. Most likely:

  * the game had not started — samples only exist once a title is running;
  * the build has no debug server — check run-info.txt for
    "Debug server listening" (both builds shipped here have it);
  * port 6545 was already in use;
  * the game was closed before it finished booting.

Run it again and stay in gameplay for at least 30 seconds before closing
Lichii. The log copied into the bundle is still useful on its own.

EOF
fi

# --------------------------------------------------------------- package
if [[ -f "$SCRIPT_DIR/analyze_bundle.py" ]] && command -v python3 >/dev/null 2>&1; then
    cp -f "$SCRIPT_DIR/analyze_bundle.py" "$OUTDIR/analyze_bundle.py" 2>/dev/null
    python3 "$SCRIPT_DIR/analyze_bundle.py" "$OUTDIR" > "$OUTDIR/analysis.txt" 2>&1
    log "summary"
    sed 's/^/  /' "$OUTDIR/analysis.txt" | head -n 40
fi

BUNDLE="$OUTDIR.zip"
if command -v zip >/dev/null 2>&1; then
    (cd "$OUTDIR/.." && zip -qr "$BUNDLE" "$(basename "$OUTDIR")")
else
    BUNDLE="$OUTDIR.tar.gz"
    tar -czf "$BUNDLE" -C "$OUTDIR/.." "$(basename "$OUTDIR")"
fi

log "done"
echo
echo "Send this file back:"
echo "  $BUNDLE"
echo
