#!/usr/bin/env bash

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${INTENTIO_BUILD_DIR:-"$ROOT_DIR/build-host"}
RESULT_DIR="$ROOT_DIR/benchmarks/results"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
RESULT_FILE="$RESULT_DIR/phase4-host-$STAMP.csv"
PLANNER_RESULT_FILE="$RESULT_DIR/phase4-planner-host-$STAMP.csv"
PHASE5_RESULT_FILE="$RESULT_DIR/phase5-host-$STAMP.csv"
METADATA_FILE="$RESULT_DIR/phase4-host-$STAMP.txt"

if ! command -v cmake >/dev/null 2>&1; then
    echo "CMake is unavailable. Install CMake 3.20+ or set up a build directory manually, then rerun this script." >&2
    exit 2
fi

mkdir -p "$RESULT_DIR"

{
    echo "timestamp_utc=$STAMP"
    echo "kernel=$(uname -srmo)"
    if command -v lscpu >/dev/null 2>&1; then
        echo "cpu_model=$(lscpu | awk -F: '/Model name/ {gsub(/^ +/, \"\", $2); print $2; exit}')"
        echo "cpu_count=$(nproc 2>/dev/null || true)"
    else
        echo "cpu_model=unavailable"
        echo "cpu_count=unavailable"
    fi
    echo "filesystem=$(stat -f -c %T "$ROOT_DIR" 2>/dev/null || echo unavailable)"
    echo "storage_source=$(df -P "$ROOT_DIR" 2>/dev/null | awk 'NR==2 {print $1}')"
    echo "cmake=$(cmake --version | head -n 1)"
} > "$METADATA_FILE"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DINTENTIO_BUILD_BENCHMARKS=ON \
    -DINTENTIO_BUILD_TESTS=ON
cmake --build "$BUILD_DIR" --parallel

PHASE4_BENCH="$BUILD_DIR/intentio_phase4_benchmark"
PLANNER_BENCH="$BUILD_DIR/intentio_planner_benchmark"
PHASE5_BENCH="$BUILD_DIR/intentio_phase5_benchmark"
if [ ! -x "$PHASE4_BENCH" ] || [ ! -x "$PLANNER_BENCH" ] ||
   [ ! -x "$PHASE5_BENCH" ]; then
    echo "Expected benchmark binaries were not produced in $BUILD_DIR" >&2
    exit 1
fi

IO_URING_CHECK=$(mktemp)
if "$PHASE4_BENCH" --workload duplicates --backend io_uring \
    --requests 1 --repetitions 1 --dedup on --coalesce on \
    >/dev/null 2>"$IO_URING_CHECK"; then
    echo "io_uring=available" | tee -a "$METADATA_FILE"
else
    echo "io_uring=unavailable: $(tr '\n' ' ' < "$IO_URING_CHECK")" | tee -a "$METADATA_FILE"
fi
rm -f "$IO_URING_CHECK"

first_row=true
for workload in duplicates hotset nested partial nonoverlap random mixed; do
    for dedup in off on; do
        for group_size in 32 128 256; do
            temporary=$(mktemp)
            "$PHASE4_BENCH" --workload "$workload" --backend auto \
                --requests "$group_size" --block-size 4096 --repetitions 7 \
                --dedup "$dedup" --coalesce on > "$temporary"
            if [ "$first_row" = true ]; then
                cp "$temporary" "$RESULT_FILE"
                first_row=false
            else
                tail -n +2 "$temporary" >> "$RESULT_FILE"
            fi
            rm -f "$temporary"
        done
    done
done

"$PLANNER_BENCH" > "$PLANNER_RESULT_FILE"
"$PHASE5_BENCH" --workload all --intent all --backend auto \
    --requests 128 --repetitions 7 > "$PHASE5_RESULT_FILE"

echo "Phase 4 benchmark results: $RESULT_FILE"
echo "Planner-only results: $PLANNER_RESULT_FILE"
echo "Phase 5 intent results: $PHASE5_RESULT_FILE"
echo "Environment metadata: $METADATA_FILE"
