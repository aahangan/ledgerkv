# ledgerkv benchmark results: reference run

The figures quoted in the README come from this run. To produce the same
report on your machine, run `make bench`; it writes `bench/results/latest.md`.

| | |
|---|---|
| Machine | Apple M5, 16 GB RAM |
| OS | macOS 26.4.1 |
| Compiler | Apple clang version 21.0.0 (clang-2100.1.1.101), RelWithDebInfo build |
| Commit | f4d9eb5 |
| Date | 2026-10-13 01:42 UTC |
| Command | `make bench` (bench/run.sh full) |

One client thread unless noted. 16-byte keys, 4 MB memtable, default level
sizes. `fillrandom` and `overwrite` each write N random keys, so about 86%
of the key space ends up live. Synced writes use F_FULLFSYNC (flushes the drive's write cache), so absolute group
commit numbers depend heavily on the disk; the speedup is the portable
figure. Throughput varies by roughly ±25% between runs with compaction
timing.

## 1.00M operations per phase, 100-byte values

| Phase | ops/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| fillseq | 323.4K | 1.6 µs | 5.8 µs | 26.2 µs |
| fillrandom | 245.1K | 1.8 µs | 7.0 µs | 137.8 µs |
| overwrite | 186.2K | 1.8 µs | 7.1 µs | 1.26 ms |
| readrandom | 130.8K | 8.2 µs | 16.1 µs | 40.0 µs |
| readmissing | 2.70M | 0.2 µs | 8.2 µs | 9.0 µs |
| readrandom8 | 656.2K | 10.5 µs | 40.8 µs | 92.1 µs |
| seekrandom | 36.2K | 24.6 µs | 42.6 µs | 84.0 µs |
| scan | 3.08M entries/s | | | |

- Tables per level after 2.00M writes: L1 20, L2 48
- Space amplification: 1.43x (137 MB on disk for 96 MB of live data)
- Write amplification: 4.21x table bytes per user byte (the WAL adds about 1x more)
- Writers stalled on background work: 1.9 s in total; 77 compactions
- Reopen (recovery) time: 52 ms
- Absent-key reads: 99.16% of 2.18M table checks were answered by the bloom filter with no disk read

### Group commit (every write fsynced before it is acknowledged)

| Concurrent writers | Writes/s | Writes per fsync | p50 latency |
|---|---|---|---|
| 1 | 309 | 1.0 | 3.07 ms |
| 4 | 680 | 2.4 | 6.06 ms |
| 16 | 2140 | 8.1 | 7.84 ms |
| 64 | 8717 | 32.2 | 7.26 ms |

### Headline figures

- **Random writes at 1.00M keys:** 245.1K/s inserting, 186.2K/s overwriting (sequential: 323.4K/s)
- **Amplification:** 4.2x write, 1.43x space
- **Random-read p99 at 1.00M keys:** 16.1 µs (p50 8.2 µs); 656.2K reads/s on 8 threads
- **Bloom filters:** 99.2% of disk reads avoided for absent keys
- **Group commit:** 28x fsynced write throughput, 309 -> 8717 writes/s at 64 writers

## 10.00M operations per phase, 100-byte values

| Phase | ops/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| fillseq | 309.8K | 1.7 µs | 5.9 µs | 31.6 µs |
| fillrandom | 115.6K | 1.8 µs | 7.8 µs | 1.27 ms |
| overwrite | 86.9K | 1.9 µs | 9.4 µs | 1.48 ms |
| readrandom | 78.1K | 8.8 µs | 111.8 µs | 179.4 µs |
| readmissing | 2.21M | 0.2 µs | 8.9 µs | 10.0 µs |
| readrandom8 | 668.1K | 10.2 µs | 41.2 µs | 97.7 µs |
| seekrandom | 29.5K | 26.3 µs | 180.2 µs | 239.7 µs |
| scan | 3.48M entries/s | | | |

- Tables per level after 20.00M writes: L0 3, L1 22, L2 101, L3 499
- Space amplification: 1.33x (1268 MB on disk for 957 MB of live data)
- Write amplification: 9.84x table bytes per user byte (the WAL adds about 1x more)
- Writers stalled on background work: 100.8 s in total; 1838 compactions
- Reopen (recovery) time: 80 ms
- Absent-key reads: 99.18% of 20.76M table checks were answered by the bloom filter with no disk read

### Headline figures

- **Random writes at 10.00M keys:** 115.6K/s inserting, 86.9K/s overwriting (sequential: 309.8K/s)
- **Amplification:** 9.8x write, 1.33x space
- **Random-read p99 at 10.00M keys:** 111.8 µs (p50 8.8 µs); 668.1K reads/s on 8 threads
- **Bloom filters:** 99.2% of disk reads avoided for absent keys

