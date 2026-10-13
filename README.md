# ledgerkv

[![ci](https://github.com/aahangan/ledgerkv/actions/workflows/ci.yml/badge.svg)](https://github.com/aahangan/ledgerkv/actions/workflows/ci.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A crash-safe key-value storage engine in C++20, the kind that sits under
databases like RocksDB and Cassandra: a log-structured merge tree with a
write-ahead log, group commit, a lock-free skiplist memtable, SSTables with
bloom filters, leveled compaction, snapshots, and iterators.

**What makes it different is how it is tested.** A fault-injection
filesystem simulates power loss and disk errors, and every recovery is
checked against a model of what must have survived.

## The fault-injection filesystem

All file I/O goes through a pluggable `Env`. In tests, it is wrapped by
[`FaultInjectionEnv`](src/fault_env.h), which keeps a ledger of what a real
disk would actually have made durable:

```
   ledgerkv ──► FaultInjectionEnv ──────────────────────► real filesystem
                │ per file:  bytes written vs. bytes fsynced
                │            directory entry fsynced yet?
                │ pending:   renames and deletes not yet made durable
                │            by a directory fsync (deleted files are parked)
                │ faults:    fail the Nth fsync / write / rename / read
                ▼
   power cut ─► rewind the disk to what could survive:
                · unsynced tails dropped, or torn and followed by garbage
                · files whose directory entry was never fsynced vanish
                · unsynced renames and deletes are undone
```

The [power-loss harness](tools/powerloss_test.cc) cuts the power at random
moments, mid-write, mid-flush, and mid-compaction, then reopens the database
and requires the recovered state to be **exactly the result of some prefix of
the writes**: no holes, no reordering, no torn batches, no wrong values. With
`sync = true`, that prefix must include every acknowledged write.

| `make powerloss` | Power cuts | Result |
|---|---|---|
| `sync = true` | 200 | every acknowledged write survived |
| `sync = false` | 600 | every recovery an exact prefix of the writes (recent unsynced writes may be lost, by design) |
| `sync = true` + one injected I/O error per cut | 300 | 160+ errors fire per run across 8 kinds (failed fsync, short write, failed rename, ...); no acknowledged write lost, and writes are refused after a WAL failure |

It found two real bugs, both fixed: with `sync = false`, a power cut could
recover a state with a *hole* in it; and fuzzing found an integer overflow
that let a crafted table file trigger an out-of-bounds read. Details are in
[Testing](#testing).

## Claims and evidence

Every figure is reproducible with one command. The reference run is in
[bench/results/reference-apple-m5.md](bench/results/reference-apple-m5.md)
(Apple M5, macOS, internal SSD).

| Claim | Measured | Reproduce | Code |
|---|---|---|---|
| Group commit raises fsynced write throughput | 28× (309 → 8,717 writes/s at 64 writers) | `make bench-quick` | [`DBImpl::Write`](src/db.cc) |
| Random writes at 10M keys | 116K/s inserting, 87K/s overwriting | `make bench` | [`src/db.cc`](src/db.cc) |
| Write and space amplification at 10M keys | 9.8× write, 1.33× space | `make bench` | [leveled compaction](#leveled-compaction) |
| Random-read latency at 1M keys | p99 16 µs (p50 8.2 µs) | `make bench-quick` | [read path](#read-path-snapshots-and-iterators) |
| Bloom filters skip disk reads for absent keys | 99.2% of table checks need no read | `make bench-quick` | [`src/bloom.cc`](src/bloom.cc) |
| Power loss never corrupts or reorders data | all 1,100 simulated power cuts recover an exact prefix of the writes | `make powerloss` | [`src/fault_env.cc`](src/fault_env.cc) |
| No acknowledged write lost with `sync = true` | 0 lost across 500 power cuts, 300 of them with an injected I/O error (160+ errors per run) | `make powerloss` | [`tools/powerloss_test.cc`](tools/powerloss_test.cc) |
| No lost writes or torn batches on process crash | `SIGKILL` mid-write, 1 and 8 writer threads | `make crash` | [`tools/crash_test.cc`](tools/crash_test.cc) |
| Decoders and API survive fuzzing | 4 targets: libFuzzer in CI, ASan + UBSan locally | `make fuzz` | [`fuzz/`](fuzz) |
| Sanitizer-clean on Linux and macOS | ASan, TSan, UBSan on every push | CI | [`ci.yml`](.github/workflows/ci.yml) |

## Status

| Milestone | Scope | State |
|---|---|---|
| 1. Durable log | WAL with CRC-32C, memtable, crash recovery, torn-tail truncation, process lock | **done** |
| 2. SSTables | Background flush to immutable sorted tables, block index, bloom filters, MANIFEST, bounded WAL | **done** |
| 3. Compaction | Leveled compaction, tombstone GC, write throttling, MANIFEST snapshots | **done** |
| 4. Concurrency | Group commit, atomic batches, lock-free skiplist memtable, snapshots, iterators | **done** |
| 5. Hardening | Pluggable `Env`, power-loss simulation with torn writes, I/O error injection, fuzzing | **done** |

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

Needs CMake 3.20+ and a C++20 compiler. No other dependencies.

```sh
make                # build (CMake underneath; see the Makefile)
make test           # 53 unit tests
make crash          # SIGKILL mid-write, 1 and 8 writer threads
make powerloss      # 1,100 simulated power cuts, 300 with injected I/O errors (~12 min)
make fuzz           # 4 fuzz targets, 20,000 mutated inputs each
make check          # all of the above
make bench-quick    # performance figures at 1M keys, including group commit (~2 min)
make bench          # plus 10M keys (~12 min); writes bench/results/latest.md
```

Sanitizer builds: `cmake -S . -B build-asan -DLEDGERKV_SANITIZE=address` (also
`thread`, `undefined`). For coverage-guided fuzzing, build with a clang that
ships libFuzzer: `cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++
-DLEDGERKV_FUZZ=ON -DLEDGERKV_SANITIZE=address`, write the seed corpus with
`./build/fuzz_seeds corpus`, then run e.g. `./build-fuzz/fuzz_db corpus/db`.

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

### Durability guarantees

These are the guarantees, stated precisely. Each one is tested by the
power-loss harness described below.

- **`sync = true`:** a write is durable before `Put`/`Write` returns. After a
  crash or power loss, every acknowledged write is present.
- **`sync = false`:** recent acknowledged writes can be lost after power loss
  (a process crash alone loses nothing). But the recovered database is always
  exactly the state after some prefix of the writes: no holes, no reordering.
  At most about one memtable's worth of writes is lost.
- **Either way:** batches are all-or-nothing, and a read never returns a
  value that was not written.

Getting the `sync = false` guarantee right took a fix found by testing.
Without per-write syncs, a power cut could keep the newer log's writes while
tearing off the older log's unsynced tail, and recovery would replay a state
with a hole in the middle. Now the retiring log is synced once whenever the
memtable is switched. That costs one fsync per 4 MB of writes, not one per
write.

**I/O errors.** A failed WAL append or fsync is reported to the writer and
makes the database refuse all later writes until it is reopened. Retrying
fsync after a failure is not safe: Linux may have already dropped the dirty
pages and will report the retry as a success (the 2018 "fsyncgate" issue in
PostgreSQL). A short write also leaves a partial record that later records
must not follow. A failed background flush or compaction is sticky in the
same way. Its inputs stay live, so nothing is lost. Read errors surface as
`IOError`, never as a wrong value or a false `NotFound`.

### Durability on macOS

`fsync()` on macOS does not flush the drive's write cache, so with
`sync = true` ledgerkv uses `fcntl(F_FULLFSYNC)`. That is the main reason synced
writes cost about 4 ms each on a laptop SSD (see the benchmarks below).

### Pluggable Env

Every filesystem call (open, append, sync, read, rename, remove, directory
sync, lock) goes through an `Env` interface. The default is POSIX.
`Options::env` can substitute another, which is how the tests inject faults.
The `Env` contract is the durability model the engine is written against:

- appended data survives power loss only after the file is synced, and
- a created, renamed, or deleted file's directory entry survives only after
  its directory is synced.

## Testing

53 unit tests, a process-crash harness, a power-loss harness, and four fuzz
targets. CI runs everything on Linux and macOS under AddressSanitizer,
ThreadSanitizer, and UBSan, plus coverage-guided libFuzzer on Linux.

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
| Skip the directory fsync after writing a table, before the MANIFEST names it | power-loss test | After the power cut the MANIFEST named a table that no longer existed |
| A failed WAL append is not sticky, so later writes go ahead | power-loss test (faults), unit tests | Writes accepted after a WAL failure |
| No sync of the retiring log at memtable switch (the original code, not a planted bug) | power-loss test, `sync = false` | Recovered state had a hole: not a prefix of the writes |
| Table footer check `offset + size + trailer == file size` can overflow (the original code, not a planted bug) | libFuzzer in CI, then a crafted-file test | Out-of-bounds read from a 100-byte crafted table, confirmed with ASan |
| Remove the bounds check from the length-prefixed field decoder | `fuzz_log`, `fuzz_write_batch`, `fuzz_table` | Heap buffer overflow, caught by ASan within seconds |
| Compaction drops a version that is one sequence number too new for the oldest snapshot | `fuzz_db` | A snapshot read returned NotFound for a key it should see (94-byte input) |

Two of those rows are bugs in the shipped code, not planted ones. The
`sync = false` hole is described under Durability guarantees. The table
reader trusted block offsets and sizes read from the file, and its footer
check could be fooled by sums that wrap past 2^64. Now every block read is
bounds-checked against the file size before anything is allocated, and the
footer checks use subtraction.

Two more findings came from checking the checks themselves. At first the
power-loss harness confirmed "writes are refused after a WAL failure" while
the injected fault was still armed, so the follow-up write failed on the
fault and proved nothing. The non-sticky bug above slipped through until the
harness was changed to clear the fault before retrying. And `fuzz_table`
missed the decoder overflow until its checksum-repairing mode was added.



`SIGKILL` cannot simulate power loss, because the page cache survives a
process crash. The power-loss harness covers that.

**Power loss** (`tools/powerloss_test.cc`, `src/fault_env.cc`). A
`FaultInjectionEnv` wraps the real filesystem and keeps a durability ledger:
for every file, how much was appended and how much was synced, and whether
its directory entry was synced; plus every rename and removal not yet made
durable by a directory sync (removed files are parked, not deleted). To cut
the power, the harness freezes the disk mid-workload, tears the database
down, and rewinds the disk to what a real one could hold:

- each file is cut back to its synced length, often keeping a random torn
  piece of the unsynced tail, sometimes followed by garbage,
- files whose directory entry was never synced disappear, and
- unsynced renames and removals are undone (the MANIFEST snapshot rename
  included).

After reopening, the recovered state must equal the model after some prefix
of the writes, and with `sync = true` that prefix must include every
acknowledged write. The harness finds the matching prefix with a running
mismatch count, updated in O(1) per operation, since each operation changes
one key. Interrupted flushes and compactions are part of every run.

With `--faults`, each power cut also comes with an injected I/O error: a
failed WAL fsync, a short WAL write, a failed table fsync, short write, or
create, a failed MANIFEST fsync, a failed MANIFEST rename during open, or a
failed directory fsync. The error must reach the caller. After a WAL failure,
writes must still be refused even once the disk "recovers". And the prefix
rule must still hold afterwards.

Latest `make powerloss` run (11.5 minutes):

| Mode | Power cuts | Operations | Result |
|---|---|---|---|
| `sync = true` | 200 | 1.53M | every acknowledged write survived |
| `sync = false` | 600 | 3.05M | every recovery an exact prefix; unsynced tail lost averaged 271 ops, max 561 (about one memtable) |
| `sync = true` + I/O errors | 300 | 1.43M | 166 injected errors across all 8 kinds, none lost an acknowledged write; all 75 WAL failures made later writes fail |

Unit tests pin down the specific cases: a WAL fsync failure is sticky and
loses nothing acknowledged; a short WAL write does not strand later writes;
failed flushes and MANIFEST commits stop writes and keep the data; and with
30% of table reads failing, 1,461 of 5,000 reads returned `IOError` and none
returned a wrong value.

**Fuzzing** (`fuzz/`). Four targets:

| Target | What it feeds | Checks |
|---|---|---|
| `fuzz_log` | WAL and MANIFEST bytes | framing, WAL payloads, and version edits never crash; decoded records survive an encode/decode round trip |
| `fuzz_write_batch` | batch encodings | decode, rebuild through the public API, decode again: same operations |
| `fuzz_table` | table files | open, scan, and lookups return data or `Corruption`, never crash. Half the inputs are wrapped in a well-formed, correctly checksummed file, so fuzzed bytes reach the block parsers instead of bouncing off the checksums. |
| `fuzz_db` | operation sequences | differential test against a `std::map`: puts, deletes, batches, flushes, full compactions, snapshots checked later, scans, seeks, reopens; every read must match, and `Verify()` must pass |

In CI they run under libFuzzer (coverage-guided, 60 s each) on Linux. Apple's
clang does not ship libFuzzer, so the same targets also link against a small
random-mutation driver that works with any compiler. Locally, each target ran
200,000 mutated inputs (20,000 for `fuzz_db`) under ASan and UBSan with no
failures.

## Benchmarks

Full results, with every phase's throughput and p50/p99/p99.9 latency, are in
[bench/results/reference-apple-m5.md](bench/results/reference-apple-m5.md),
generated by `make bench` at commit `f4d9eb5` on an Apple M5 (16 GB RAM,
macOS 26.4, internal SSD). Run `make bench` to get the same report for your
machine. One client thread unless noted, 16-byte keys, 100-byte values, 4 MB
memtable. Throughput varies by roughly ±25% between runs with compaction
timing.

**Group commit** (every write fsynced with `F_FULLFSYNC` before it is
acknowledged):

| Concurrent writers | Writes/s | Writes per fsync | p50 latency |
|---|---|---|---|
| 1 | 309 | 1.0 | 3.1 ms |
| 4 | 680 | 2.4 | 6.1 ms |
| 16 | 2,140 | 8.1 | 7.8 ms |
| 64 | **8,717** | 32.2 | 7.3 ms |

Throughput grows almost linearly with the number of writers while each
writer's latency stays near the cost of two fsyncs: the one already in flight
when it arrives, and its own group's.

**Summary:**

| | 1M operations per phase | 10M operations per phase |
|---|---|---|
| Random inserts / overwrites | 245K/s / 186K/s | 116K/s / 87K/s |
| Sequential inserts | 323K/s | 310K/s |
| Random reads, 1 thread (p50 / p99) | 131K/s (8.2 µs / 16 µs) | 78K/s (8.8 µs / 112 µs) |
| Random reads, 8 threads | 656K/s | 668K/s |
| Absent-key reads | 2.70M/s | 2.21M/s |
| Bloom filters: absent-key table checks needing no disk read | 99.2% | 99.2% |
| Full scan | 3.08M entries/s | 3.48M entries/s |
| Write / space amplification | 4.2× / 1.43× | 9.8× / 1.33× |
| Reopen (recovery) | 52 ms | 80 ms |

What these numbers say:
- **Group commit is the big win for durable writes.** A single fsynced
  writer is capped at one `F_FULLFSYNC` (~3–4 ms) per write. Concurrent
  writers share fsyncs, so 64 of them get 28× the throughput.
- **Reads scale across threads** (5–9× on 8 threads) because `Get` holds a
  lock only long enough to copy three pointers, the memtable is lock-free,
  and tables are read with `pread()`. At 10M keys, single-threaded reads wait
  on the SSD, so eight threads overlap those waits and scale further.
- **Compaction trades write throughput for reads and space.** Before
  compaction (milestone 2), random writes ran at 445K/s, but 1M keys took
  2× their size on disk and a read could check 85 tables. Now a read checks
  at most a few level-0 tables plus one per level.
- **Random writes are compaction-bound at 10M.** A single background thread
  writes ~10× the user data, and writers spent 101 s stalled over the two
  10M-write phases, which is where the 1–2 ms p99.9 comes from. Running
  compactions on more threads, or limiting how much a compaction can overlap
  the level two below (as LevelDB does), would be the next steps.
- **Read p99 at 10M is disk-bound.** ~10 GB of compaction writes churn the
  page cache, so the tail includes SSD reads. There is no block cache yet.
- **The hardening layer cost nothing measurable.** Routing all I/O through a
  virtual `Env`, and syncing the retiring log at each memtable switch, left
  every benchmark within normal run-to-run variance.

## License

MIT. See [LICENSE](LICENSE).
