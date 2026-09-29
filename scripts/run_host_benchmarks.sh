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
PHASE9_FADVISE_RESULT_FILE="$RESULT_DIR/phase9-fadvise-host-$STAMP.csv"
PHASE9_DONTCACHE_RESULT_FILE="$RESULT_DIR/phase9-dontcache-host-$STAMP.csv"
PHASE9_CACHE_RESULT_FILE="$RESULT_DIR/phase9-cache-pollution-host-$STAMP.csv"
PHASE9_CONCURRENCY_RESULT_FILE="$RESULT_DIR/phase9-concurrency-host-$STAMP.csv"
PHASE10_INFO_RESULT_FILE="$RESULT_DIR/phase10-capability-host-$STAMP.txt"
PHASE10_CORRECTNESS_RESULT_FILE="$RESULT_DIR/phase10-correctness-host-$STAMP.csv"
PHASE10_SEQUENTIAL_RESULT_FILE="$RESULT_DIR/phase10-direct-sequential-host-$STAMP.csv"
PHASE10_RANDOM_RESULT_FILE="$RESULT_DIR/phase10-direct-random-host-$STAMP.csv"
PHASE10_PLANNER_RESULT_FILE="$RESULT_DIR/phase10-planner-direct-host-$STAMP.csv"
PHASE10_DUPLICATES_RESULT_FILE="$RESULT_DIR/phase10-duplicates-host-$STAMP.csv"
PHASE10_REREAD_RESULT_FILE="$RESULT_DIR/phase10-reread-host-$STAMP.csv"
PHASE10_CACHE_RESULT_FILE="$RESULT_DIR/phase10-cache-pressure-host-$STAMP.csv"
PHASE10_ADAPTIVE_RESULT_FILE="$RESULT_DIR/phase10-adaptive-host-$STAMP.csv"
PHASE10_THRESHOLD_RESULT_FILE="$RESULT_DIR/phase10-threshold-host-$STAMP.csv"
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
PHASE9_BENCH="$BUILD_DIR/intentio_phase9_benchmark"
PHASE10_BENCH="$BUILD_DIR/intentio_phase10_benchmark"
if [ ! -x "$PHASE4_BENCH" ] || [ ! -x "$PLANNER_BENCH" ] ||
   [ ! -x "$PHASE5_BENCH" ] || [ ! -x "$PHASE6_BENCH" ] ||
   [ ! -x "$PHASE7_BENCH" ] || [ ! -x "$PHASE8_INFO" ] ||
   [ ! -x "$PHASE9_BENCH" ] || [ ! -x "$PHASE10_BENCH" ]; then
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

first_phase9_fadvise_row=true
for policy in baseline sequential noreuse; do
    for request_size in 4096 65536 262144 1048576; do
        temporary=$(mktemp)
        "$PHASE9_BENCH" --mode scan --policy "$policy" --workload sequential \
            --qd 16 --workers 16 --requests 64 --request-size "$request_size" \
            --repetitions 1 --materialize-file > "$temporary"
        if [ "$first_phase9_fadvise_row" = true ]; then
            cp "$temporary" "$PHASE9_FADVISE_RESULT_FILE"
            first_phase9_fadvise_row=false
        else
            tail -n +2 "$temporary" >> "$PHASE9_FADVISE_RESULT_FILE"
        fi
        rm -f "$temporary"
    done
done

first_phase9_dontcache_row=true
for workload in random4k random64k mixed sequential; do
    for policy in baseline random noreuse dontcache; do
        temporary=$(mktemp)
        "$PHASE9_BENCH" --mode scan --policy "$policy" --workload "$workload" \
            --qd 16 --workers 16 --requests 128 --repetitions 1 \
            --materialize-file > "$temporary"
        if [ "$first_phase9_dontcache_row" = true ]; then
            cp "$temporary" "$PHASE9_DONTCACHE_RESULT_FILE"
            first_phase9_dontcache_row=false
        else
            tail -n +2 "$temporary" >> "$PHASE9_DONTCACHE_RESULT_FILE"
        fi
        rm -f "$temporary"
    done
done

first_phase9_cache_row=true
for materialize_flag in --materialize-file ""; do
    temporary=$(mktemp)
    "$PHASE9_BENCH" --mode cache-pollution --qd 16 --workers 16 \
        --hot-mib 4 --hot-requests 1024 --scan-mib 64 \
        $materialize_flag > "$temporary"
    if [ "$first_phase9_cache_row" = true ]; then
        cp "$temporary" "$PHASE9_CACHE_RESULT_FILE"
        first_phase9_cache_row=false
    else
        tail -n +2 "$temporary" >> "$PHASE9_CACHE_RESULT_FILE"
    fi
    rm -f "$temporary"
done

first_phase9_concurrency_row=true
for workload in random4k sequential; do
    for policy in baseline noreuse dontcache; do
        for qd in 1 4 16 64; do
            temporary=$(mktemp)
            "$PHASE9_BENCH" --mode scan --policy "$policy" --workload "$workload" \
                --qd "$qd" --workers 64 --requests 64 --repetitions 1 \
                --materialize-file > "$temporary"
            if [ "$first_phase9_concurrency_row" = true ]; then
                cp "$temporary" "$PHASE9_CONCURRENCY_RESULT_FILE"
                first_phase9_concurrency_row=false
            else
                tail -n +2 "$temporary" >> "$PHASE9_CONCURRENCY_RESULT_FILE"
            fi
            rm -f "$temporary"
        done
    done
done

"$PHASE8_INFO" "$ROOT_DIR" > "$PHASE10_INFO_RESULT_FILE"

first_phase10_correctness_row=true
for policy in buffered dontcache direct; do
    temporary=$(mktemp)
    "$PHASE10_BENCH" --mode correctness --policy "$policy" \
        --qd 4 --workers 4 --materialize-file > "$temporary"
    if [ "$first_phase10_correctness_row" = true ]; then
        cp "$temporary" "$PHASE10_CORRECTNESS_RESULT_FILE"
        first_phase10_correctness_row=false
    else
        tail -n +2 "$temporary" >> "$PHASE10_CORRECTNESS_RESULT_FILE"
    fi
    rm -f "$temporary"
done

first_phase10_sequential_row=true
for policy in buffered dontcache direct; do
    for request_size in 4096 65536 262144 1048576 4194304; do
        for qd in 1 4 16 64; do
            temporary=$(mktemp)
            "$PHASE10_BENCH" --mode scan --policy "$policy" \
                --workload sequential --qd "$qd" --workers 64 --requests 64 \
                --request-size "$request_size" --producers 4 \
                --repetitions 1 --materialize-file > "$temporary"
            if [ "$first_phase10_sequential_row" = true ]; then
                cp "$temporary" "$PHASE10_SEQUENTIAL_RESULT_FILE"
                first_phase10_sequential_row=false
            else
                tail -n +2 "$temporary" >> "$PHASE10_SEQUENTIAL_RESULT_FILE"
            fi
            rm -f "$temporary"
        done
    done
done

first_phase10_random_row=true
for workload in random4k random64k; do
    for policy in buffered dontcache direct; do
        for qd in 1 4 16 64; do
            temporary=$(mktemp)
            "$PHASE10_BENCH" --mode scan --policy "$policy" \
                --workload "$workload" --qd "$qd" --workers 64 --requests 128 \
                --producers 4 --repetitions 1 --materialize-file > "$temporary"
            if [ "$first_phase10_random_row" = true ]; then
                cp "$temporary" "$PHASE10_RANDOM_RESULT_FILE"
                first_phase10_random_row=false
            else
                tail -n +2 "$temporary" >> "$PHASE10_RANDOM_RESULT_FILE"
            fi
            rm -f "$temporary"
        done
    done
done

first_phase10_planner_row=true
for policy in buffered dontcache direct; do
    for qd in 1 4 16 64; do
        temporary=$(mktemp)
        "$PHASE10_BENCH" --mode planner --policy "$policy" \
            --qd "$qd" --workers 64 --requests 128 --materialize-file > "$temporary"
        if [ "$first_phase10_planner_row" = true ]; then
            cp "$temporary" "$PHASE10_PLANNER_RESULT_FILE"
            first_phase10_planner_row=false
        else
            tail -n +2 "$temporary" >> "$PHASE10_PLANNER_RESULT_FILE"
        fi
        rm -f "$temporary"
    done
done

first_phase10_duplicate_row=true
for policy in buffered dontcache direct; do
    temporary=$(mktemp)
    "$PHASE10_BENCH" --mode duplicates --policy "$policy" \
        --qd 16 --workers 64 --requests 4096 --materialize-file > "$temporary"
    if [ "$first_phase10_duplicate_row" = true ]; then
        cp "$temporary" "$PHASE10_DUPLICATES_RESULT_FILE"
        first_phase10_duplicate_row=false
    else
        tail -n +2 "$temporary" >> "$PHASE10_DUPLICATES_RESULT_FILE"
    fi
    rm -f "$temporary"
done

first_phase10_reread_row=true
for policy in buffered dontcache direct; do
    temporary=$(mktemp)
    "$PHASE10_BENCH" --mode reread --policy "$policy" \
        --workload sequential --qd 16 --workers 64 --requests 64 \
        --request-size 1048576 --producers 4 --materialize-file > "$temporary"
    if [ "$first_phase10_reread_row" = true ]; then
        cp "$temporary" "$PHASE10_REREAD_RESULT_FILE"
        first_phase10_reread_row=false
    else
        tail -n +2 "$temporary" >> "$PHASE10_REREAD_RESULT_FILE"
    fi
    rm -f "$temporary"
done

first_phase10_cache_row=true
for policy in buffered dontcache direct; do
    temporary=$(mktemp)
    "$PHASE10_BENCH" --mode cache-pressure --policy "$policy" \
        --qd 16 --workers 64 --hot-mib 4 --hot-requests 1024 --scan-mib 64 \
        --materialize-file > "$temporary"
    if [ "$first_phase10_cache_row" = true ]; then
        cp "$temporary" "$PHASE10_CACHE_RESULT_FILE"
        first_phase10_cache_row=false
    else
        tail -n +2 "$temporary" >> "$PHASE10_CACHE_RESULT_FILE"
    fi
    rm -f "$temporary"
done

first_phase10_adaptive_row=true
for policy in buffered dontcache direct; do
    temporary=$(mktemp)
    "$PHASE10_BENCH" --mode scan --policy "$policy" \
        --workload sequential --qd 64 --workers 64 --requests 128 \
        --request-size 1048576 --producers 4 --adaptive --materialize-file > "$temporary"
    if [ "$first_phase10_adaptive_row" = true ]; then
        cp "$temporary" "$PHASE10_ADAPTIVE_RESULT_FILE"
        first_phase10_adaptive_row=false
    else
        tail -n +2 "$temporary" >> "$PHASE10_ADAPTIVE_RESULT_FILE"
    fi
    rm -f "$temporary"
done

first_phase10_threshold_row=true
for threshold in 65536 262144 1048576; do
    for request_size in 4096 65536 262144 1048576 4194304; do
        temporary=$(mktemp)
        "$PHASE10_BENCH" --mode scan --policy threshold \
            --direct-threshold "$threshold" --workload sequential --qd 16 \
            --workers 64 --requests 64 --request-size "$request_size" \
            --producers 4 --repetitions 1 --materialize-file > "$temporary"
        if [ "$first_phase10_threshold_row" = true ]; then
            cp "$temporary" "$PHASE10_THRESHOLD_RESULT_FILE"
            first_phase10_threshold_row=false
        else
            tail -n +2 "$temporary" >> "$PHASE10_THRESHOLD_RESULT_FILE"
        fi
        rm -f "$temporary"
    done
done

echo "Phase 4 benchmark results: $RESULT_FILE"
echo "Planner-only results: $PLANNER_RESULT_FILE"
echo "Phase 5 intent results: $PHASE5_RESULT_FILE"
echo "Phase 6 concurrency results: $PHASE6_RESULT_FILE"
echo "Phase 7 executor results: $PHASE7_EXECUTOR_RESULT_FILE"
echo "Phase 7 batch-size results: $PHASE7_BATCH_RESULT_FILE"
echo "Phase 7 workload results: $PHASE7_WORKLOAD_RESULT_FILE"
echo "Phase 7 fairness results: $PHASE7_FAIRNESS_RESULT_FILE"
echo "Phase 8 io_uring capability: $PHASE8_CAPABILITY_RESULT_FILE"
echo "Phase 9 fadvise results: $PHASE9_FADVISE_RESULT_FILE"
echo "Phase 9 RWF_DONTCACHE results: $PHASE9_DONTCACHE_RESULT_FILE"
echo "Phase 9 cache-pollution results: $PHASE9_CACHE_RESULT_FILE"
echo "Phase 9 concurrency results: $PHASE9_CONCURRENCY_RESULT_FILE"
echo "Phase 10 capability: $PHASE10_INFO_RESULT_FILE"
echo "Phase 10 correctness: $PHASE10_CORRECTNESS_RESULT_FILE"
echo "Phase 10 sequential results: $PHASE10_SEQUENTIAL_RESULT_FILE"
echo "Phase 10 random results: $PHASE10_RANDOM_RESULT_FILE"
echo "Phase 10 planner results: $PHASE10_PLANNER_RESULT_FILE"
echo "Phase 10 duplicate results: $PHASE10_DUPLICATES_RESULT_FILE"
echo "Phase 10 reread results: $PHASE10_REREAD_RESULT_FILE"
echo "Phase 10 cache-pressure results: $PHASE10_CACHE_RESULT_FILE"
echo "Phase 10 adaptive results: $PHASE10_ADAPTIVE_RESULT_FILE"
echo "Phase 10 threshold results: $PHASE10_THRESHOLD_RESULT_FILE"
echo "Environment metadata: $METADATA_FILE"
