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
PHASE6_RESULT_FILE="$RESULT_DIR/phase6-host-$STAMP.csv"
PHASE7_EXECUTOR_RESULT_FILE="$RESULT_DIR/phase7-executor-host-$STAMP.csv"
PHASE7_BATCH_RESULT_FILE="$RESULT_DIR/phase7-batch-host-$STAMP.csv"
PHASE7_WORKLOAD_RESULT_FILE="$RESULT_DIR/phase7-workload-host-$STAMP.csv"
PHASE7_FAIRNESS_RESULT_FILE="$RESULT_DIR/phase7-fairness-host-$STAMP.csv"
PHASE8_CAPABILITY_RESULT_FILE="$RESULT_DIR/phase8-capability-host-$STAMP.txt"
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
PHASE6_BENCH="$BUILD_DIR/intentio_phase6_benchmark"
PHASE7_BENCH="$BUILD_DIR/intentio_phase7_benchmark"
PHASE8_INFO="$BUILD_DIR/intentio-info"
if [ ! -x "$PHASE4_BENCH" ] || [ ! -x "$PLANNER_BENCH" ] ||
   [ ! -x "$PHASE5_BENCH" ] || [ ! -x "$PHASE6_BENCH" ] ||
   [ ! -x "$PHASE7_BENCH" ] || [ ! -x "$PHASE8_INFO" ]; then
    echo "Expected benchmark binaries were not produced in $BUILD_DIR" >&2
    exit 1
fi

"$PHASE8_INFO" "$ROOT_DIR" | tee "$PHASE8_CAPABILITY_RESULT_FILE"

IO_URING_CHECK=$(mktemp)
if "$PHASE6_BENCH" --workload random4k --backend io_uring --mode fixed \
    --qd 1 --workers 1 --requests 1 --repetitions 1 \
    >/dev/null 2>"$IO_URING_CHECK"; then
    echo "io_uring=available" | tee -a "$METADATA_FILE"
    IO_URING_AVAILABLE=true
else
    echo "io_uring=unavailable: $(tr '\n' ' ' < "$IO_URING_CHECK")" | tee -a "$METADATA_FILE"
    IO_URING_AVAILABLE=false
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

first_phase6_row=true
phase6_backends=(threaded)
if [ "$IO_URING_AVAILABLE" = true ]; then
    phase6_backends+=(io_uring)
fi
for backend in "${phase6_backends[@]}"; do
    for workload in random4k random64k sequential intent mixed; do
        for qd in 1 2 4 8 16 32 64 128; do
            temporary=$(mktemp)
            "$PHASE6_BENCH" --workload "$workload" --backend "$backend" \
                --mode fixed --qd "$qd" --workers 128 --intent unknown \
                --requests 128 --repetitions 7 > "$temporary"
            if [ "$first_phase6_row" = true ]; then
                cp "$temporary" "$PHASE6_RESULT_FILE"
                first_phase6_row=false
            else
                tail -n +2 "$temporary" >> "$PHASE6_RESULT_FILE"
            fi
            rm -f "$temporary"
        done
        temporary=$(mktemp)
        "$PHASE6_BENCH" --workload "$workload" --backend "$backend" \
            --mode adaptive --max-qd 128 --workers 128 --intent unknown \
            --requests 128 --repetitions 7 > "$temporary"
        tail -n +2 "$temporary" >> "$PHASE6_RESULT_FILE"
        rm -f "$temporary"
    done
    for intent in sequential latency throughput; do
        temporary=$(mktemp)
        "$PHASE6_BENCH" --workload intent --backend "$backend" \
            --mode adaptive --max-qd 128 --workers 128 --intent "$intent" \
            --requests 128 --repetitions 7 > "$temporary"
        tail -n +2 "$temporary" >> "$PHASE6_RESULT_FILE"
        rm -f "$temporary"
    done
    temporary=$(mktemp)
    "$PHASE6_BENCH" --workload changing --backend "$backend" \
        --mode adaptive --max-qd 128 --workers 128 --intent unknown \
        --requests 128 --repetitions 7 > "$temporary"
    tail -n +2 "$temporary" >> "$PHASE6_RESULT_FILE"
    rm -f "$temporary"
done

first_phase7_executor_row=true
for producers in 1 2 4 8 16; do
    for workers in 1 2 4 8 16 32 64 128; do
        temporary=$(mktemp)
        "$PHASE7_BENCH" --mode executor --backend threaded \
            --workers "$workers" --qd "$workers" --producers "$producers" \
            --groups 64 --requests 64 > "$temporary"
        if [ "$first_phase7_executor_row" = true ]; then
            cp "$temporary" "$PHASE7_EXECUTOR_RESULT_FILE"
            first_phase7_executor_row=false
        else
            tail -n +2 "$temporary" >> "$PHASE7_EXECUTOR_RESULT_FILE"
        fi
        rm -f "$temporary"
    done
done

first_phase7_batch_row=true
for batch_size in 1 2 4 8 16; do
    temporary=$(mktemp)
    "$PHASE7_BENCH" --mode executor --backend threaded \
        --workers 8 --qd 8 --executor-batch-size "$batch_size" \
        --producers 4 --groups 32 --requests 64 > "$temporary"
    if [ "$first_phase7_batch_row" = true ]; then
        cp "$temporary" "$PHASE7_BATCH_RESULT_FILE"
        first_phase7_batch_row=false
    else
        tail -n +2 "$temporary" >> "$PHASE7_BATCH_RESULT_FILE"
    fi
    rm -f "$temporary"
done

first_phase7_workload_row=true
for qd in 1 2 4 8 16 32 64 128; do
    for workload in random4k random64k sequential mixed; do
        temporary=$(mktemp)
        "$PHASE7_BENCH" --mode fixed --backend threaded --qd "$qd" \
            --max-qd "$qd" --workers 128 --producers 4 --groups 32 \
            --requests 128 --workload "$workload" > "$temporary"
        if [ "$first_phase7_workload_row" = true ]; then
            cp "$temporary" "$PHASE7_WORKLOAD_RESULT_FILE"
            first_phase7_workload_row=false
        else
            tail -n +2 "$temporary" >> "$PHASE7_WORKLOAD_RESULT_FILE"
        fi
        rm -f "$temporary"
    done
done
for workload in random4k random64k sequential mixed; do
    temporary=$(mktemp)
    "$PHASE7_BENCH" --mode adaptive --backend threaded --max-qd 128 \
        --workers 128 --producers 4 --groups 128 --requests 256 \
        --workload "$workload" > "$temporary"
    tail -n +2 "$temporary" >> "$PHASE7_WORKLOAD_RESULT_FILE"
    rm -f "$temporary"
done
temporary=$(mktemp)
"$PHASE7_BENCH" --mode changing --backend threaded --max-qd 128 \
    --workers 128 --producers 4 --groups 64 --requests 256 > "$temporary"
tail -n +2 "$temporary" >> "$PHASE7_WORKLOAD_RESULT_FILE"
rm -f "$temporary"

"$PHASE7_BENCH" --mode fairness --backend threaded --qd 8 --workers 8 \
    --groups 32 --requests 64 > "$PHASE7_FAIRNESS_RESULT_FILE"

echo "Phase 4 benchmark results: $RESULT_FILE"
echo "Planner-only results: $PLANNER_RESULT_FILE"
echo "Phase 5 intent results: $PHASE5_RESULT_FILE"
echo "Phase 6 concurrency results: $PHASE6_RESULT_FILE"
echo "Phase 7 executor results: $PHASE7_EXECUTOR_RESULT_FILE"
echo "Phase 7 batch-size results: $PHASE7_BATCH_RESULT_FILE"
echo "Phase 7 workload results: $PHASE7_WORKLOAD_RESULT_FILE"
echo "Phase 7 fairness results: $PHASE7_FAIRNESS_RESULT_FILE"
echo "Phase 8 io_uring capability: $PHASE8_CAPABILITY_RESULT_FILE"
echo "Environment metadata: $METADATA_FILE"
