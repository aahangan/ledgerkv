# ledgerkv

A crash-safe, persistent key-value store in C++20, built from scratch as a
log-structured merge tree: a write-ahead log, an in-memory memtable, sorted
on-disk tables with bloom filters, and leveled compaction.

The goal is correctness you can check, not peak speed. Each durability claim
here is backed by a test that tries to break it.

## Status

| Milestone | Scope | State |
|---|---|---|
| 1. Durable log | WAL with CRC-32C, memtable, crash recovery, torn-tail truncation, process lock | **done** |
| 2. SSTables | Background flush to immutable sorted tables, block index, bloom filters, MANIFEST, bounded WAL | **done** |
| 3. Compaction | Leveled compaction, tombstone GC, write throttling, MANIFEST snapshots | **done** |
| 4. Concurrency | Group commit, snapshots, iterators | next |
| 5. Hardening | Power-loss simulation (drop unsynced writes), fsync-failure injection, fuzzing | planned |

## API

```cpp
#include "ledgerkv/db.h"

ledgerkv::Options options;          // options.sync = true by default
std::unique_ptr<ledgerkv::DB> db;
ledgerkv::Status s = ledgerkv::DB::Open(options, "/tmp/mydb", &db);

db->Put("user:42", "alice");
std::string value;
s = db->Get("user:42", &value);  // s.IsNotFound() if absent
db->Delete("user:42");

db->Flush();                     // Force the memtable to disk now
db->CompactAll();                // Push everything to the bottom level
db->Verify();                    // Check level invariants and every checksum
db->GetStats();                  // Per-level sizes, write amplification inputs
```

## Build and test

```sh
cmake -S . -B build && cmake --build build -j
./build/ledgerkv_tests              # unit tests
./build/crash_test --iterations 200
./build/kvbench --num 1000000    # add --sync to fsync every write
```

Sanitizer builds: `cmake -S . -B build-asan -DLEDGERKV_SANITIZE=address` (also
`thread`, `undefined`).

## Design

```
 Put/Delete ─► WAL ─► memtable ─full─► immutable memtable ─flush─► L0  (tables may overlap)
                                                                    │ compaction
                                                                    ▼
 Get ─► memtable ─► immutable ─► L0 newest first ─► L1 ─► L2 ... L6 (each level sorted, no overlap)
```

### Files

| File | Contents |
|---|---|
| `NNNNNN.log` | Write-ahead log for the current memtable (and the one being flushed, if any) |
| `NNNNNN.sst` | Immutable sorted table |
| `MANIFEST` | Log of version edits: tables added and removed, current log number, counters |
| `MANIFEST.tmp` | A MANIFEST snapshot being written; renamed over `MANIFEST` when complete |
| `LOCK` | `flock()`ed by the process that has the database open |
| `lost/` | Tables set aside during recovery because the MANIFEST tail was damaged |

The WAL and MANIFEST share one record framing:

```
| crc32c (4) | len (4) | payload (len bytes) |
```

The checksum covers the length field as well as the payload, so a corrupted
length cannot make the reader skip into the middle of another record.

### Write path

`Put` and `Delete` take a writer mutex, assign the next sequence number,
append a record to the WAL, optionally `fsync` it, then apply it to the
memtable. A write is acknowledged only after its record is in the log.

When the memtable reaches `write_buffer_size` (4 MB by default), it is
frozen, a new log is started, and the background thread flushes the frozen
memtable to a level-0 table. Writers keep going into the new memtable.

If background work falls behind, writers are throttled rather than allowed to
build an unbounded backlog. At 8 level-0 tables each write is delayed by 1 ms.
At 12, or if the memtable fills again before the previous flush finishes,
writes block until the background thread catches up.

### Tables

```
[data block]...[data block][bloom filter][index][footer: offsets + magic]
```

- Data blocks (~4 KB) hold sorted entries: key, `seq << 8 | type`, value.
  Deletes are stored as tombstones so they hide older values in deeper levels.
- The index holds each block's last key, so a lookup reads at most one block.
- A per-table bloom filter (10 bits/key, 7 probes, double hashing) skips
  tables that cannot contain the key. Measured false-positive rate: 0.92%.
- Every block carries a CRC-32C, checked on every read.

The index and filter are loaded into memory when a table is opened. Data
blocks are read with `pread()` and rely on the OS page cache; there is no
block cache yet.

### Leveled compaction

Level 0 holds tables straight from flushes, so their key ranges overlap.
Every level from 1 down is a single sorted run: tables in the same level never
overlap. Level L may hold 10 MB × 10^(L-1) before it must be compacted into
level L+1.

The background thread scores each level: level-0 table count over the
trigger (4), or level size over its limit. The highest score of at least 1 is
compacted:

- **Level 0:** all level-0 tables, plus every level-1 table overlapping them.
- **Level L ≥ 1:** one table, chosen round-robin through the key space, plus
  the level-L+1 tables it overlaps.

The inputs are merged with a k-way heap ordered by key, then by sequence
number, newest first. Only the newest entry for each key is kept. The output
is split into ~2 MB tables at key boundaries, so output tables never share a
key. A lone input with nothing to merge against is moved down a level by a
MANIFEST edit alone, with no rewrite (a "trivial move").

**Tombstones** are dropped only when no deeper level has a table whose range
covers the key. Until then a tombstone may be the only thing hiding an older
value, and dropping it would bring that value back. One test exists just to
catch this, and it caught an injected bug that drops tombstones too early.

Flushes take priority over compaction. A running compaction checks every
4,096 entries for a pending memtable and flushes it first. Without that,
writers would stall behind a long merge.

### Read path

`Get` grabs the current memtable, the frozen memtable, and the current
`Version` (an immutable, reference-counted snapshot of every level) under a
brief lock, then searches without holding any lock:

1. memtable, then frozen memtable
2. level-0 tables whose range covers the key, newest first
3. one table per deeper level, found by binary search on key ranges

The first layer with an entry for the key decides, so a tombstone in a newer
layer hides older values. Flushes and compactions install a new `Version`
instead of modifying the old one. A reader holding an old `Version` can still
read tables a compaction has since deleted, because POSIX keeps an unlinked
file alive while it is open.

### Crash safety of background work

Flushes and compactions follow the same order:

1. Write the new tables and `fsync` them, then `fsync` the directory.
2. Append one MANIFEST edit that adds the new tables, removes the inputs, and
   advances the log number. `fsync` it.
3. Delete the inputs and any fully flushed logs.

The MANIFEST edit is a single checksummed record, so it is applied all or
nothing. A crash before step 2 leaves orphan outputs, which are deleted on
the next open, while the inputs and old logs remain live. A crash after step 2
leaves garbage that is deleted on open. At no point is data in neither a
live table nor a live log. I checked this by swapping steps 2 and 3, for both
flushes and compactions. The crash test caught both (details under Testing).

Compaction outputs still being written are tracked so that a flush running in
the middle of a compaction does not delete them as orphans.

### Recovery

1. Replay the MANIFEST to rebuild the live table set. A torn tail is dropped.
   A record that passes its checksum but does not parse is real corruption,
   so open fails rather than guess.
2. Replay every log at or above the recorded log number, oldest first,
   truncating torn tails so new appends never sit behind garbage.
3. Flush the replayed writes to a level-0 table.
4. Write the full state as a single-record snapshot to `MANIFEST.tmp`,
   `fsync` it, and `rename()` it over `MANIFEST`. The rename is atomic, so a
   crash leaves the old MANIFEST or the new one, never a mix.
5. Delete the old logs, orphan tables, and leftover temp files.

Since logs are deleted once flushed, recovery replays at most about one
memtable of log, however large the database is: **75 ms** to reopen a 1.26 GB
database. The MANIFEST is also rewritten as a snapshot whenever it passes
4 MB, so it stays small too.

If an append fails partway, the log may end in a partial record, so the
database rejects all later writes until it is reopened. A failed background
flush or compaction is sticky in the same way.

**Known limitation:** bit rot in the last MANIFEST record cannot be told
apart from a torn append, so that edit is dropped. Recovery then moves the
tables the MANIFEST does not mention into `lost/` instead of deleting them, so
the data is still on disk and can be recovered by hand, but it is not served.

### Durability on macOS

`fsync()` on macOS does not flush the drive's write cache, so with
`sync = true` ledgerkv uses `fcntl(F_FULLFSYNC)`. That is the main reason synced
writes cost about 4 ms each on a laptop SSD (see the benchmarks below).

## Testing

31 unit tests, plus a crash harness. CI runs everything on Linux and macOS
under AddressSanitizer, ThreadSanitizer, and UBSan.

**Torn and corrupted writes.** The WAL tests truncate the log at every byte
offset inside the final record and flip every single bit in the log.
Recovery must return exactly the intact prefix: never a partial record, and
never anything after the damage. The table tests flip bytes throughout a
table and check that every read either fails with `Corruption` or returns the
correct value, never a wrong one. The MANIFEST tests cover a torn tail, a
damaged last record, and a record that checksums but does not parse.

**Compaction.** A randomized test runs 60,000 puts and deletes against a
`std::map` model with tiny level sizes, reopening periodically, and checks
every key plus `Verify()` throughout. Other tests check that a full compaction
reclaims overwritten space, that tombstones are dropped at the bottom level
and kept while an older value lies below, and that readers racing
compactions always see exact values.

**Crash test** (`tools/crash_test.cc`). A child process applies a
deterministic stream of puts and deletes and reports each acknowledged
operation over a pipe. After a random delay the parent `SIGKILL`s it, reopens
the database, and checks keys against a model:

- every acknowledged write is present with the right value,
- nothing past the in-flight operation is visible, and
- the in-flight operation is all-or-nothing.

The memtable (256 KB) and level sizes are kept small so that kills keep
landing in the middle of flushes and compactions. The final iteration also
runs `Verify()`. A typical run: **100 crashes, 60 of them during a flush,
474K operations, 265 tables, zero acknowledged writes lost.**

To check that the tests can catch real bugs, I injected these:

| Injected bug | Caught by | Result |
|---|---|---|
| Buffer WAL writes in user space, so writes are acknowledged before they reach the kernel | crash test | Failed on the first crash |
| Delete the flushed log before the MANIFEST edit commits the new table | crash test | Failed within one 100-crash run |
| Delete compaction inputs before the MANIFEST edit commits the outputs | crash test | Failed within one 100-crash run |
| Drop tombstones in every compaction, not only at the bottom | model test, tombstone test | Deleted keys came back |

What `SIGKILL` does not cover: power loss, where unsynced data in the page
cache is lost. That is milestone 5.

## Benchmarks

Apple M5 (16 GB RAM), macOS 26.4, internal SSD, one client thread, 16-byte
keys, 100-byte values, 4 MB memtable, default level sizes. Latency is per
operation. `fillrandom` and `overwrite` each write N random keys, so about
86% of the key space ends up live.

**10M operations per phase** (957 MB live data):

| Workload | ops/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| fillseq | 317K | 1.6 µs | 5.3 µs | 37 µs |
| fillrandom | 112K | 1.8 µs | 6.9 µs | 1.5 ms |
| overwrite | 85K | 1.8 µs | 7.5 µs | 1.9 ms |
| readrandom | 79K | 8.8 µs | 115 µs | 179 µs |
| readmissing (bloom filters) | 1.40M | 0.33 µs | 9.1 µs | 16 µs |

| | |
|---|---|
| Shape after 20M writes | L0 2 tables, L1 20, L2 100, L3 499 |
| Space amplification | 1.32× (1.26 GB on disk for 957 MB live) |
| Write amplification | 10.4× table bytes per user byte, plus ~1× for the WAL |
| Reopen | 75 ms |

**1M operations per phase** (96 MB live data):

| Workload | ops/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| fillrandom | 258K | 1.7 µs | 5.9 µs | 203 µs |
| overwrite | 196K | 1.8 µs | 6.5 µs | 1.3 ms |
| readrandom | 133K | 8.3 µs | 13 µs | 28 µs |
| readmissing | 3.6M | 0.17 µs | 8.2 µs | 8.8 µs |
| fillrandom, `sync = true` | 255 | 3.95 ms | 20.7 ms | 36 ms |

Space amp 1.49×, write amp 4.3×, reopen 34 ms.

What these numbers say:
- **Compaction trades write throughput for reads and space.** Before
  compaction (milestone 2), random writes ran at 445K/s but 1M keys took
  2× their size on disk, and a read could check 85 tables. Now a read checks
  at most a few level-0 tables plus one per level, and p99 read latency at 1M
  keys dropped from 30 µs to 13 µs.
- **Random writes are compaction-bound at 10M.** A single background thread
  writes ~10× the user data, and writers spent 114 s stalled over the two
  10M-write phases, which is where the 1–2 ms p99.9 comes from. Running
  compactions on more threads, or limiting how much a compaction can overlap
  the level two below (as LevelDB does), would be the next steps.
- **readrandom p99 at 10M is disk-bound.** The p50 hit the page cache, but
  ~10 GB of compaction writes churned the cache, so the tail includes SSD
  reads. There is no block cache yet.
- **Synced writes** are bound by one `F_FULLFSYNC` per write. Group commit
  (milestone 4) should amortize that across concurrent writers.
