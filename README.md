# ledgerkv

A crash-safe, persistent key-value store in C++20, built from scratch as a
log-structured merge tree: a write-ahead log, an in-memory memtable, and
sorted on-disk tables with bloom filters. Compaction is next.

The goal is correctness you can check, not peak speed. Each durability claim
here is backed by a test that tries to break it.

## Status

| Milestone | Scope | State |
|---|---|---|
| 1. Durable log | WAL with CRC-32C, memtable, crash recovery, torn-tail truncation, process lock | **done** |
| 2. SSTables | Background flush to immutable sorted tables, block index, bloom filters, MANIFEST, bounded WAL | **done** |
| 3. Compaction | Leveled compaction in a background thread, tombstone GC | next |
| 4. Concurrency | Group commit, snapshots, iterators | planned |
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
db->Flush();                     // Optional: force the memtable to disk now
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
 Put/Delete ──► WAL (000007.log) ──► memtable ──full──► immutable memtable ──► 000008.sst
                                                        (background thread)       │
 Get ──► memtable ──► immutable memtable ──► tables, newest first ◄────────────────┘
                                                        MANIFEST records which tables are live
```

### Files

| File | Contents |
|---|---|
| `NNNNNN.log` | Write-ahead log for the current memtable (and the one being flushed, if any) |
| `NNNNNN.sst` | Immutable sorted table |
| `MANIFEST` | Log of version edits: tables added, current log number, next file number, last sequence |
| `LOCK` | `flock()`ed by the process that has the database open |

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
frozen, a new log is started, and a background thread flushes the frozen
memtable to a table. Writers keep going into the new memtable. They block only
if it also fills before the previous flush finishes.

### Tables

```
[data block]...[data block][bloom filter][index][footer: offsets + magic]
```

- Data blocks (~4 KB) hold sorted entries: key, `seq << 8 | type`, value.
  Deletes are stored as tombstones so they hide older values in older tables.
- The index holds each block's last key, so a lookup reads at most one block.
- A per-table bloom filter (10 bits/key, 7 probes, double hashing) skips
  tables that cannot contain the key. Measured false-positive rate: 0.92%.
- Every block carries a CRC-32C, checked on every read.

The index and filter are loaded into memory when a table is opened. Data
blocks are read with `pread()` and rely on the OS page cache; there is no
block cache yet.

### Read path

`Get` grabs the current memtable, the frozen memtable, and the current
`Version` (an immutable, reference-counted list of tables) under a brief lock,
then searches them newest to oldest without holding any lock. The first layer
with an entry for the key decides, so a tombstone in a newer layer hides a
value in an older one. A flush installs a new `Version` instead of modifying
the old one, so readers never see a half-updated table list.

### Crash safety of a flush

The order of operations is what makes a flush crash-safe:

1. Write the table and `fsync` it, then `fsync` the directory.
2. Append a MANIFEST edit adding the table and advancing the log number, and `fsync` it.
3. Delete the logs below the new log number.

A crash before step 2 leaves an orphan table, which is deleted on the next
open, and the old log is still there to replay. A crash after step 2 leaves a
committed table, and any stale log is deleted on open. At no point is a write
in neither a committed table nor a live log. I checked this by moving step 3
before step 2. The crash test caught it straight away (details under Testing).

### Recovery

1. Replay the MANIFEST to rebuild the live table set. A torn tail is dropped
   and truncated. A record that passes its checksum but does not parse is
   real corruption, so open fails rather than guess.
2. Replay every log at or above the recorded log number, oldest first,
   truncating torn tails so new appends never sit behind garbage.
3. Flush the replayed writes to a table and commit it, then delete the old
   logs.

Since logs are deleted once flushed, recovery replays at most about one
memtable of log, however large the database is: **34 ms** to reopen a 240 MB
database.

If an append fails partway, the log may end in a partial record, so the
database rejects all later writes until it is reopened. A failed background
flush is sticky in the same way.

**Known limitation:** bit rot in the last MANIFEST record cannot be told
apart from a torn append, so that edit is dropped. In that case recovery
keeps the tables the MANIFEST does not mention instead of deleting them as
orphans, so the data is still on disk and can be recovered by hand, but it is
not served.

### Durability on macOS

`fsync()` on macOS does not flush the drive's write cache, so with
`sync = true` ledgerkv uses `fcntl(F_FULLFSYNC)`. That is the main reason synced
writes cost about 4 ms each on a laptop SSD (see the benchmarks below).

## Testing

25 unit tests, plus a crash harness. CI runs everything on Linux and macOS
under AddressSanitizer, ThreadSanitizer, and UBSan.

**Torn and corrupted writes.** The WAL tests truncate the log at every byte
offset inside the final record and flip every single bit in the log.
Recovery must return exactly the intact prefix: never a partial record, and
never anything after the damage. The table tests flip bytes throughout a
table and check that every read either fails with `Corruption` or returns the
correct value, never a wrong one. The MANIFEST tests cover a torn tail, a
damaged last record, and a record that checksums but does not parse.

**Crash test** (`tools/crash_test.cc`). A child process applies a
deterministic stream of puts and deletes and reports each acknowledged
operation over a pipe. After a random delay the parent `SIGKILL`s it, reopens
the database, and checks keys against a model:

- every acknowledged write is present with the right value,
- nothing past the in-flight operation is visible, and
- the in-flight operation is all-or-nothing.

The memtable is kept small (256 KB) so that kills keep landing during
background flushes. A typical run: **100 crashes, 74 of them during a flush,
553K operations, 376 tables, zero acknowledged writes lost.**

To check that the harness can catch bugs, I injected two:

| Injected bug | Result |
|---|---|
| Buffer WAL writes in user space, so writes are acknowledged before they reach the kernel | Failed on the first crash |
| Delete the flushed log before the MANIFEST edit commits the new table | Failed within one 100-crash run |

What `SIGKILL` does not cover: power loss, where unsynced data in the page
cache is lost. That is milestone 5.

## Benchmarks

Apple M5, macOS 26.4, internal SSD, one thread, 16-byte keys, 100-byte
values, 1M operations per phase, 4 MB memtable. Latency is per operation.

| Workload | ops/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| fillrandom | 445K | 1.6 µs | 4.0 µs | 10 µs |
| overwrite | 442K | 1.6 µs | 4.1 µs | 12 µs |
| readrandom (from 85 tables on disk) | 89K | 9.2 µs | 30 µs | 159 µs |
| readmissing (bloom filters) | 392K | 2.5 µs | 3.1 µs | 7.1 µs |
| fillrandom, `sync = true` | 255 | 3.95 ms | 20.7 ms | 36 ms |

| | |
|---|---|
| Reopen after 2M writes (240 MB on disk) | 34 ms |
| Writer stall time waiting on flushes | 0 ms |

Caveats:
- With no compaction yet, overwrites pile up: 1M live keys take 240 MB
  on disk (about 2× the logical size), and a read may check all 85 tables.
  Bloom filters keep a miss down to about 0.85 wasted block reads per lookup,
  but this is the cost milestone 3 exists to fix.
- The data fits in the OS page cache, so these are warm-cache reads.
- Synced writes are bound by one `F_FULLFSYNC` per write. Group commit
  (milestone 4) should amortize that across concurrent writers.
