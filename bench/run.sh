#!/usr/bin/env bash
# Reproduces every performance figure in the README and writes a report.
#
#   bench/run.sh [quick|full] [build-dir]
#
#   quick  1M operations per phase, including group commit   (~2 minutes)
#   full   quick, plus 10M operations per phase              (~12 minutes)
#
# The report goes to bench/results/latest.md. Reference results from the
# machine the README quotes are in bench/results/reference-apple-m5.md.
set -euo pipefail
cd "$(dirname "$0")/.."

mode="${1:-full}"
build="${2:-build}"
out="bench/results/latest.md"
mkdir -p bench/results

if [ "$(uname)" = Darwin ]; then
  cpu="$(sysctl -n machdep.cpu.brand_string)"
  mem="$(( $(sysctl -n hw.memsize) / 1073741824 )) GB RAM"
  os="macOS $(sw_vers -productVersion)"
  fsync="F_FULLFSYNC (flushes the drive's write cache)"
else
  cpu="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')"
  mem="$(( $(awk '/MemTotal/ {print $2}' /proc/meminfo) / 1048576 )) GB RAM"
  os="$(uname -sr)"
  fsync="fsync"
fi
build_type="$(grep -m1 '^CMAKE_BUILD_TYPE' "$build/CMakeCache.txt" | cut -d= -f2)"
compiler="$(grep -m1 '^CMAKE_CXX_COMPILER:' "$build/CMakeCache.txt" | cut -d= -f2)"
compiler="$("$compiler" --version | head -1)"
commit="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
# Only code changes matter for the numbers; ignore documentation edits.
git diff --quiet HEAD -- src include bench tools CMakeLists.txt Makefile 2>/dev/null ||
  commit="$commit (with uncommitted code changes)"

cat > "$out" <<HEADER
# ledgerkv benchmark results

| | |
|---|---|
| Machine | $cpu, $mem |
| OS | $os |
| Compiler | $compiler, $build_type build |
| Commit | $commit |
| Date | $(date -u +"%Y-%m-%d %H:%M UTC") |
| Command | \`make bench\` (bench/run.sh $mode) |

One client thread unless noted. 16-byte keys, 4 MB memtable, default level
sizes. \`fillrandom\` and \`overwrite\` each write N random keys, so about 86%
of the key space ends up live. Synced writes use $fsync, so absolute group
commit numbers depend heavily on the disk; the speedup is the portable
figure. Throughput varies by roughly ±25% between runs with compaction
timing.

HEADER

"$build/kvbench" --num 1000000 --report "$out"
if [ "$mode" = full ]; then
  "$build/kvbench" --num 10000000 --no-sync-writes --report "$out"
fi
echo
echo "Report written to $out"
