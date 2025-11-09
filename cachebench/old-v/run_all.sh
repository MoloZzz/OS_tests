set -e

echo "test,run,time,cache-misses,cycles,instructions" > results_cachebench.csv

for run in {1..3}; do
    echo "=== RUN $run ==="
    sudo perf stat -e cache-misses,cycles,instructions ./cachebench 2>&1 \
    | grep "STAT:" | while IFS= read -r line; do
        test=$(echo $line | cut -d: -f2 | cut -d, -f1)
        time=$(echo $line | grep -o "time=[0-9.]*" | cut -d= -f2)
        echo "RUN: $test ($time s)"
        # Capture perf stats
        stats=$(sudo perf stat -e cache-misses,cycles,instructions ./cachebench 2>&1 | grep -E "cache-misses|cycles|instructions" | awk '{print $1}' | tr '\n' ',')
        echo "$test,$run,$time,$stats" >> results_cachebench.csv
    done
done

echo "Results saved to results_cachebench.csv"
