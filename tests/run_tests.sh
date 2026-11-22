#!/bin/bash
# Tests libnvcodec_container_fix.so on a GPU machine: the same checks run first without the library as a baseline, then
# with it preloaded. Nothing outside the repository is changed; the test clip and the encoder output go to a scratch
# directory under tmp/ that is removed at the end.
#   bash tests/run_tests.sh
# Needs nvidia-smi, plus (both can be overridden through the environment):
#   PYTHON  Python 3 for tests/test_cuda.py, standard library only (default: python3)
#   FFMPEG  ffmpeg with NVDEC/NVENC support: hevc_cuvid, -hwaccel cuda, h264_nvenc and hevc_nvenc (default: ffmpeg).
#           The HEVC test clip for the decoders is encoded with libx265 if this ffmpeg has it, otherwise with
#           hevc_nvenc with the library preloaded
# The baseline also sets NVCODEC_CONTAINER_FIX_DISABLE=1, so that it stays a baseline when the library is preloaded
# through /etc/ld.so.preload. The baseline is informational; the script exits non-zero if any check fails with the
# library preloaded.
cd "$(dirname "$0")/.." || exit 1
LIB=$PWD/libnvcodec_container_fix.so
PYTHON=${PYTHON:-python3}
FFMPEG=${FFMPEG:-ffmpeg}
[ -f "$LIB" ] || { echo "$LIB not found, run make first"; exit 1; }
for tool in nvidia-smi "$PYTHON" "$FFMPEG"; do
    command -v "$tool" >/dev/null || { echo "$tool not found"; exit 1; }
done
GPU_INDEXES=$(nvidia-smi --query-gpu=index --format=csv,noheader) || { echo "nvidia-smi could not list the GPUs"; exit 1; }
NUM_GPUS=$(grep -c . <<<"$GPU_INDEXES")
[ "$NUM_GPUS" -gt 0 ] || { echo "nvidia-smi lists no GPUs"; exit 1; }
OUT=$(mkdir -p tmp && mktemp -d "$PWD/tmp/run.XXXXXX") || exit 1
trap 'rm -rf "$OUT"' EXIT
echo "host $(hostname), gpus: $(nvidia-smi --query-gpu=index,pci.bus_id --format=csv,noheader | tr '\n' ';')"

SOURCE=(-f lavfi -i testsrc=size=1280x720:rate=60)
VIDEO=$OUT/testsrc_hevc.mp4  # 5 s, 300 frames
encode_clip() { "$FFMPEG" -hide_banner -v error -y "${SOURCE[@]}" -t 5 -pix_fmt yuv420p "$@" "$VIDEO"; }
if "$FFMPEG" -hide_banner -encoders 2>/dev/null | grep -qw libx265; then
    encode_clip -c:v libx265 -preset ultrafast -x265-params log-level=error
else
    LD_PRELOAD=$LIB encode_clip -c:v hevc_nvenc
fi || { echo "could not encode the HEVC test clip with $FFMPEG"; exit 1; }

# run <name> <command...>: print the exit code and the last two lines of output (debug log lines removed), and count
# a failure in failures
run() {
    local name=$1; shift
    local output code
    output=$("$@" 2>&1); code=$?
    printf '  %-26s exit=%-3s %s\n' "$name" "$code" "$(echo "$output" | grep -v '^\[nvcodec_container_fix\]' | tail -2 | tr '\n' ' ' | cut -c1-170)"
    [ "$code" -eq 0 ] || failures=$((failures + 1))
}

suite() {
    run "cuda" "$PYTHON" -I tests/test_cuda.py
    run "nvidia-smi" nvidia-smi --query-gpu=index,pci.bus_id,uuid --format=csv,noheader
    for ((gpu = 0; gpu < NUM_GPUS; gpu++)); do
        run "ffmpeg hevc_cuvid gpu$gpu" "$FFMPEG" -hide_banner -v error -c:v hevc_cuvid -gpu "$gpu" -i "$VIDEO" -f null -
        run "ffmpeg -hwaccel cuda gpu$gpu" "$FFMPEG" -hide_banner -v error -hwaccel cuda -hwaccel_device "$gpu" -hwaccel_output_format cuda -i "$VIDEO" -f null -
        run "ffmpeg h264_nvenc gpu$gpu" "$FFMPEG" -hide_banner -v error -y "${SOURCE[@]}" -t 5 -c:v h264_nvenc -gpu "$gpu" "$OUT/h264_$gpu.mp4"
        run "ffmpeg hevc_nvenc gpu$gpu" "$FFMPEG" -hide_banner -v error -y "${SOURCE[@]}" -t 5 -c:v hevc_nvenc -gpu "$gpu" "$OUT/hevc_$gpu.mp4"
        run "decode nvenc output gpu$gpu" "$FFMPEG" -hide_banner -v error -c:v hevc_cuvid -gpu "$gpu" -i "$OUT/hevc_$gpu.mp4" -f null -
    done
}

echo "[without LD_PRELOAD]"
(
    unset LD_PRELOAD
    export NVCODEC_CONTAINER_FIX_DISABLE=1
    suite
)
echo "[with LD_PRELOAD]"
failures=0
LD_PRELOAD=$LIB suite
echo "[with LD_PRELOAD, debug log: hevc_cuvid, hevc_nvenc]"
LD_PRELOAD=$LIB NVCODEC_CONTAINER_FIX_DEBUG=1 "$FFMPEG" -hide_banner -v error -c:v hevc_cuvid -i "$VIDEO" -f null - 2>&1 | grep nvcodec_container_fix | sort | uniq -c
LD_PRELOAD=$LIB NVCODEC_CONTAINER_FIX_DEBUG=1 "$FFMPEG" -hide_banner -v error -y "${SOURCE[@]}" -t 1 -c:v hevc_nvenc "$OUT/debug.mp4" 2>&1 | grep nvcodec_container_fix | sort | uniq -c
if [ "$failures" -gt 0 ]; then
    echo "$failures checks failed with LD_PRELOAD"
    exit 1
fi
echo "all checks passed with LD_PRELOAD"
