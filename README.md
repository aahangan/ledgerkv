# ledgerkv

A crash-safe, persistent key-value store in C++20, built from scratch as a
log-structured merge tree: a write-ahead log with group commit, a lock-free
skiplist memtable, sorted on-disk tables with bloom filters, leveled
compaction, snapshots, and iterators.

The goal is correctness you can check, not peak speed. Each durability claim
here is backed by a test that tries to break it.

## Status

| Milestone | Scope | State |
|---|---|---|
| 1. Durable log | WAL with CRC-32C, memtable, crash recovery, torn-tail truncation, process lock | **done** |
| 2. SSTables | Background flush to immutable sorted tables, block index, bloom filters, MANIFEST, bounded WAL | **done** |
| 3. Compaction | Leveled compaction, tombstone GC, write throttling, MANIFEST snapshots | **done** |
| 4. Concurrency | Group commit, atomic batches, lock-free skiplist memtable, snapshots, iterators | **done** |
| 5. Hardening | Power-loss simulation (drop unsynced writes), fsync-failure injection, fuzzing | next |

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

// Atomic batch: all or nothing, even across a crash.
ledgerkv::WriteBatch batch;
batch.Put("balance:alice", "70");
batch.Put("balance:bob", "130");
db->Write(batch);

// Point-in-time reads.
const ledgerkv::Snapshot* snap = db->GetSnapshot();
ledgerkv::ReadOptions at_snap;
at_snap.snapshot = snap;
db->Get(at_snap, "balance:alice", &value);  // Unaffected by later writes
db->ReleaseSnapshot(snap);

// Ordered scans over a consistent view.
auto it = db->NewIterator();
for (it->Seek("user:"); it->Valid() && it->key().starts_with("user:"); it->Next()) {
  // it->key(), it->value()
}

db->Flush();       // Force the memtable to disk now
db->CompactAll();  // Push everything to the bottom level and drop garbage
db->Verify();      // Check level invariants and every checksum
db->GetStats();    // Per-level sizes, amplification inputs, group commit sizes
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
 writers ─► queue ─leader─► WAL (one write + one fsync per group) ─► skiplist memtable
                                                                          │ full
                                                       immutable memtable ┘─flush─► L0 (tables may overlap)
                                                                                     │ compaction
                                                                                     ▼
 Get / Iterator ─► memtable ─► immutable ─► L0 newest first ─► L1 ─► ... L6 (each level sorted, no overlap)
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

### Write path and group commit

Every write is a `WriteBatch` (`Put` and `Delete` wrap a one-operation
batch). Writers join a queue, and the writer at the front becomes the
**leader**. The leader:

1. gathers the batches queued behind it (up to 1 MB, or 128 KB past a small
   leading batch, so one tiny write is not held up behind megabytes),
2. assigns them consecutive sequence numbers,
3. releases the lock and appends them all to the WAL with **one `write()` and
   one `fsync`**, each batch as its own checksummed record,
4. inserts every operation into the memtable,
5. re-takes the lock, publishes the new last sequence number, and wakes the
   writers it committed.

While the leader waits on the disk, new writers keep queueing, and the next
leader takes them all at once. So the more writers are waiting, the more
writes each fsync covers: with 64 writers, each fsync carried 32 writes.

**Atomicity.** A batch is one WAL record, so after a crash all of it is
present or none of it is. Readers never see a batch half applied, because
each read uses the last *published* sequence number as its implicit
snapshot, and the leader publishes only after the whole group is in the
memtable.

When the memtable reaches `write_buffer_size` (4 MB by default), it is
frozen, a new log is started, and the background thread flushes the frozen
memtable to a level-0 table. Writers keep going into the new memtable.

If background work falls behind, writers are throttled rather than allowed to
build an unbounded backlog. At 8 level-0 tables each write group is delayed
by 1 ms. At 12, or if the memtable fills again before the previous flush
finishes, writes block until the background thread catches up.

### Memtable

The memtable is a skiplist that supports one writer alongside any number of
**lock-free readers**. Nodes are allocated from an arena and never modified
or freed while the memtable is alive. Each node is fully built before a
release-store links it in, and readers traverse with acquire-loads, so a
reader sees each node either not at all or complete. The single-writer rule
comes for free, since only the write leader inserts.

Entries are encoded once into the arena as `key | seq << 8 | type | value`
and ordered by key, then by sequence descending. Every version of a key is
kept, which is what lets snapshots read old values.

### Tables

```
[data block]...[data block][bloom filter][index][footer: offsets + magic]
```

- Data blocks (~4 KB) hold entries in internal order (key ascending, then
  sequence descending): key, `seq << 8 | type`, value. A key can appear
  several times when snapshots still need its old versions. Deletes are
  stored as tombstones so they hide older values in deeper levels.
- The index holds each block's last *internal* key (key and sequence), so a
  lookup of "newest version of `k` at or below sequence `s`" reads at most
  one block.
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
number, newest first. The output is split into ~2 MB tables at key
boundaries, so output tables never share a key. A lone input with nothing to
merge against is moved down a level by a MANIFEST edit alone, with no rewrite
(a "trivial move").

**Old versions** are dropped only when no reader can see them. An entry is
garbage when a newer version of the same key is already visible to the
oldest live snapshot: every reader stops at that newer version. Without
snapshots, that keeps only the newest version of each key. The same rule
applies when a memtable is flushed. `CompactAll()` finishes by rewriting the
bottom level in place, so versions pinned by since-released snapshots do not
linger there.

**Tombstones** are dropped only when every snapshot can see them and no
deeper level has a table whose range covers the key. Until then a tombstone
may be the only thing hiding an older value, and dropping it would bring
that value back. One test exists just to
catch this, and it caught an injected bug that drops tombstones too early.

Flushes take priority over compaction. A running compaction checks every
4,096 entries for a pending memtable and flushes it first. Without that,
writers would stall behind a long merge.

### Read path, snapshots, and iterators

`Get` grabs the current memtable, the frozen memtable, the current `Version`
(an immutable, reference-counted snapshot of every level), and a sequence
number (the snapshot's, or else the last published one) under a brief lock.
It then searches without holding any lock:

1. memtable, then frozen memtable
2. level-0 tables whose range covers the key, newest first
3. one table per deeper level, found by binary search on key ranges

In each place it looks for the newest version of the key with a sequence at
or below its own. The first layer with such a version decides, so a tombstone
in a newer layer hides older values.

A **snapshot** is just a sequence number, registered so that compaction keeps
the versions it can see. Taking one is O(log n) and copies nothing.

An **iterator** merges one child per memtable, one per level-0 table, and one
per deeper level (each a sorted run, opened table by table) with a heap. On
top of that it shows, for each key, only the newest version at or below its
sequence, and skips keys whose newest such version is a tombstone. An
iterator pins the memtables and `Version` it started with, so writes,
flushes, and compactions after it was created never change what it returns.

Flushes and compactions install a new `Version` instead of modifying the old
one. A reader holding an old `Version` can still read tables a compaction has
since deleted, because POSIX keeps an unlinked file alive while it is open.

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

40 unit tests, plus a crash harness. CI runs everything on Linux and macOS
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

**Concurrency, snapshots, and iterators.** Three reader threads traverse the
skiplist while one thread inserts 50,000 keys, and must always see a sorted
list. One thread writes two keys in the same batch 20,000 times while another
reads them through snapshots and iterators; the two values never disagree. A
snapshot must keep returning old values through flushes and full compactions,
and the space must come back once it is released. Iterator scans and seeks
are checked against a `std::map` across the memtable and every level. An open
iterator must not change while the database is rewritten under it. Sixteen
threads of synced writers must all commit, sharing fsyncs.

**Crash test** (`tools/crash_test.cc`). A child process runs one or more
writer threads, each applying a deterministic stream of puts, deletes, and
two-key batches and reporting every acknowledged operation over a pipe. After
a random delay the parent `SIGKILL`s it, reopens the database, and checks,
per thread, against a model:

- every acknowledged write is present with the right value,
- nothing past the in-flight operation is visible,
- the in-flight operation is all-or-nothing, and
- both keys of every batch are in the same state.

The memtable (256 KB) and level sizes are kept small so that kills keep
landing in the middle of flushes and compactions. With several threads, they
also land in the middle of group commits. The final iteration runs
`Verify()`. Typical runs:

| Writers | Crashes | During a flush | Operations | Lost acknowledged writes | Torn batches |
|---|---|---|---|---|---|
| 1 | 100 | 54 | 373K | 0 | 0 |
| 8 | 100 | 19 | 92K | 0 | 0 |

To check that the tests can catch real bugs, I injected these:

| Injected bug | Caught by | Result |
|---|---|---|
| Buffer WAL writes in user space, so writes are acknowledged before they reach the kernel | crash test | Failed on the first crash |
| Delete the flushed log before the MANIFEST edit commits the new table | crash test | Failed within one 100-crash run |
| Delete compaction inputs before the MANIFEST edit commits the outputs | crash test | Failed within one 100-crash run |
| Drop tombstones in every compaction, not only at the bottom | model test, tombstone test | Deleted keys came back |
| Group commit acknowledges followers before their records reach the WAL | crash test, 8 writers | Acknowledged writes lost |
| Compaction ignores live snapshots when dropping old versions | snapshot test | Snapshot read returned the new value |

What `SIGKILL` does not cover: power loss, where unsynced data in the page
cache is lost. That is milestone 5.

## Benchmarks

Apple M5 (16 GB RAM), macOS 26.4, internal SSD, 16-byte keys, 100-byte
values, 4 MB memtable, default level sizes. Latency is per operation. One
client thread unless noted. `fillrandom` and `overwrite` each write N random
keys, so about 86% of the key space ends up live. Write throughput varies by
roughly ±25% from run to run with compaction timing.

**Group commit** (every write fsynced with `F_FULLFSYNC` before it is
acknowledged):

| Concurrent writers | Writes/s | Writes per fsync | p50 latency |
|---|---|---|---|
| 1 | 270 | 1.0 | 4.0 ms |
| 4 | 656 | 2.4 | 6.1 ms |
| 16 | 2,226 | 8.2 | 7.0 ms |
| 64 | **8,589** | 32.0 | 7.2 ms |

Throughput grows almost linearly with the number of writers (32× at 64) while
each writer's latency stays near the cost of two fsyncs: the one already in
flight when it arrives, and its own group's.

**10M operations per phase** (957 MB live data):

| Workload | ops/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| fillseq | 322K | 1.6 µs | 5.7 µs | 27 µs |
| fillrandom | 117K | 1.8 µs | 7.6 µs | 1.3 ms |
| overwrite | 84K | 1.9 µs | 9.4 µs | 1.5 ms |
| readrandom | 87K | 8.6 µs | 99 µs | 217 µs |
| readrandom, 8 threads | 629K | 10 µs | 52 µs | 181 µs |
| readmissing (bloom filters) | 2.27M | 0.25 µs | 8.8 µs | 11 µs |
| seek + 10 × next | 34K | 26 µs | 47 µs | 131 µs |
| full scan | 3.09M entries/s (341 MB/s) | | | |

| | |
|---|---|
| Shape after 20M writes | L0 3 tables, L1 21, L2 101, L3 499 |
| Space amplification | 1.33× (1.27 GB on disk for 957 MB live) |
| Write amplification | 9.8× table bytes per user byte, plus ~1× for the WAL |
| Reopen | 77 ms |

**1M operations per phase** (96 MB live data):

| Workload | ops/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| fillrandom | 200K | 1.9 µs | 12 µs | 336 µs |
| overwrite | 185K | 1.8 µs | 7.2 µs | 1.3 ms |
| readrandom | 132K | 8.3 µs | 16 µs | 38 µs |
| readrandom, 8 threads | 669K | 11 µs | 38 µs | 84 µs |
| readmissing | 2.72M | 0.21 µs | 8.3 µs | 9.0 µs |
| seek + 10 × next | 36K | 25 µs | 46 µs | 118 µs |
| full scan | 3.46M entries/s (382 MB/s) | | | |

Space amp 1.45×, write amp 4.2×, reopen 39 ms.

What these numbers say:
- **Group commit is the big win for durable writes.** A single fsynced
  writer is capped at one `F_FULLFSYNC` (~4 ms) per write. Concurrent writers
  share fsyncs, so 64 of them get 32× the throughput.
- **Reads scale across threads** (5–7× on 8 threads) because `Get` holds a
  lock only long enough to copy three pointers, the memtable is lock-free,
  and tables are read with `pread()`. The shared lock and reference counts
  are what keep it short of 8×.
- **Compaction trades write throughput for reads and space.** Before
  compaction (milestone 2), random writes ran at 445K/s, but 1M keys took
  2× their size on disk and a read could check 85 tables. Now a read checks
  at most a few level-0 tables plus one per level.
- **Random writes are compaction-bound at 10M.** A single background thread
  writes ~10× the user data, and writers spent 105 s stalled over the two
  10M-write phases, which is where the 1–2 ms p99.9 comes from. Running
  compactions on more threads, or limiting how much a compaction can overlap
  the level two below (as LevelDB does), would be the next steps.
- **readrandom p99 at 10M is disk-bound.** ~10 GB of compaction writes churn
  the page cache, so the tail includes SSD reads. There is no block cache yet.
- **Multi-version storage cost little.** Moving from a `std::map` that kept
  one version per key to a skiplist that keeps every version left
  single-threaded throughput about where it was in milestone 3.
