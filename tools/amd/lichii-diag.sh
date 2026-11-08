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
GAME_ARGS=()
PORT_STR="127.0.0.1:6545"
EXTRA_SWITCHES=()

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  --binary PATH       Lichii binary or AppImage (or set LICHII=/path/to/lichii)
  --outdir DIR        where to write the bundle (default: ./lichii-diag-<time>)
  --duration SECS     stop after SECS instead of waiting for Lichii to exit
  --switch NAME=VAL   extra EDEN_NX2_* switch for this run (repeatable)
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
        -h|--help) usage; exit 0 ;;
        --) shift; GAME_ARGS=("$@"); break ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

log() { printf '\033[1m==>\033[0m %s\n' "$*"; }

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
LICHII_PID=""

cleanup() {
    [[ -n "$PROBE_PID" ]] && kill "$PROBE_PID" 2>/dev/null
    [[ -n "$GPU_MON_PID" ]] && kill "$GPU_MON_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

# The driver pipeline cache only grows when a pipeline had to be built, so a
# before/after snapshot separates compilation stutter from plain load.
snapshot_shader_cache() {
    local out="$1"
    : > "$out"
    for base in "${XDG_DATA_HOME:-$HOME/.local/share}/eden/cache/shader" \
                "$HOME/.local/share/eden/cache/shader"; do
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
else
    echo "vulkaninfo not installed" > "$OUTDIR/sysinfo/vulkaninfo.txt"
fi

display_switch=""
if [[ "${XDG_SESSION_TYPE:-}" == "wayland" ]]; then
    display_switch="--enable-qt-wayland"
fi

# ----------------------------------------------------------------- switches
# Keep the profile cheap: the point is frame timing, not per-draw tracing.
export EDEN_NX2_DEBUG_SERVER="$PORT_STR"
export EDEN_NX2_GPU_PROFILE=1
export EDEN_NX2_PIPELINE_BUILD_TRACE="$OUTDIR/pipeline_build.log"

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
"$BINARY" "${EXTRA[@]}" "${GAME_ARGS[@]}" \
    > "$OUTDIR/lichii-stdout.log" 2>&1 &
LICHII_PID=$!

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

# ------------------------------------------------------------------- probe
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
[[ -n "$PROBE_PID" ]] && kill "$PROBE_PID" 2>/dev/null
[[ -n "$GPU_MON_PID" ]] && kill "$GPU_MON_PID" 2>/dev/null
PROBE_PID=""
GPU_MON_PID=""

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
    echo "samples: $(wc -l < "$OUTDIR/frametimes.jsonl" 2>/dev/null || echo 0)"
    echo "marks: $(grep -c '"mark": true' "$OUTDIR/frametimes.jsonl" 2>/dev/null || true)"
    [[ -f "$OUTDIR/shader-cache-delta.txt" ]] && cat "$OUTDIR/shader-cache-delta.txt"
} > "$OUTDIR/SUMMARY.txt" 2>&1

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
