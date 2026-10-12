# ledgerkv

A crash-safe, persistent key-value store in C++20, built from scratch as a
log-structured merge tree: a write-ahead log, an in-memory memtable, sorted
on-disk tables, and background compaction.

The goal is correctness you can check, not peak speed. Each durability claim
here is backed by a test that tries to break it.

## Status

| Milestone | Scope | State |
|---|---|---|
| 1. Durable log | WAL with CRC-32C, memtable, crash recovery, torn-tail truncation, process lock | **done** |
| 2. SSTables | Flush memtable to immutable sorted files, block index, bloom filters, MANIFEST, bounded WAL | next |
| 3. Compaction | Leveled compaction in a background thread, tombstone GC | planned |
| 4. Concurrency | Group commit, lock-free reads via ref-counted versions, snapshots, iterators | planned |
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
```

## Build and test

```sh
cmake -S . -B build && cmake --build build -j
./build/ledgerkv_tests              # unit tests
./build/crash_test --iterations 200
./build/kvbench --num 200000     # add --sync to fsync every write
```

Sanitizer builds: `cmake -S . -B build-asan -DLEDGERKV_SANITIZE=address` (also
`thread`, `undefined`).

## Design

### Write path

`Put` and `Delete` take a writer mutex, assign the next sequence number,
append a record to the WAL, optionally `fsync` it, then apply it to the
memtable. A write is acknowledged only after its record is in the log.

WAL record format:

```
| crc32c (4) | len (4) | type (1) | seq (8) | varint klen | key | varint vlen | value |
                       '------------------ len bytes -------------------------'
```

The checksum covers the length field as well as the payload, so a corrupted
length cannot make the reader skip into the middle of another record.

### Recovery

On open, the WAL is replayed from the start and stops at the first record
that is truncated or fails its checksum. The file is then truncated to the
last intact record before any new writes are appended. Without that step,
new records would sit behind the torn bytes, and the next recovery would stop
early and silently drop them.

If an append fails partway, the log may end in a partial record, so the
database rejects all later writes until it is reopened and recovered.

### Durability on macOS

`fsync()` on macOS does not flush the drive's write cache, so with
`sync = true` ledgerkv uses `fcntl(F_FULLFSYNC)`. That is the main reason synced
writes cost about 4 ms each on a laptop SSD (see the benchmarks below).

## Testing

**Torn-write tests** (`tests/wal_test.cc`) truncate the log at every byte
offset inside the final record, and separately flip every single bit in the
log. Recovery must return exactly the intact prefix: never a partial record,
and never anything after the damage.

**Crash test** (`tools/crash_test.cc`) forks a child that applies a
deterministic stream of puts and deletes and reports each acknowledged
operation over a pipe. After a random delay the parent `SIGKILL`s it, reopens
the database, and checks every key against a model:

- every acknowledged write is present with the right value,
- nothing past the in-flight operation is visible, and
- the in-flight operation is all-or-nothing.

To check that the harness can catch bugs, I injected one (buffering WAL writes
in user space so writes are acknowledged before they reach the kernel). The
crash test failed on the first iteration.

What `SIGKILL` does not cover: power loss, where unsynced data in the page cache is
lost. That's milestone 5.

## Benchmarks

Apple M5, macOS 26.4, internal SSD, 16-byte keys, 100-byte values, one
thread. Latency is per operation.

| Workload | sync | ops/s | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| fillrandom | off | 549k | 1.7 µs | 3.8 µs | 9.2 µs |
| overwrite | off | 486k | 1.8 µs | 4.1 µs | 15 µs |
| readrandom | – | 2.2M | 0.4 µs | 0.8 µs | 1.0 µs |
| fillrandom | on | 255 | 3.95 ms | 20.7 ms | 36 ms |

Caveats: everything fits in memory at this stage, so reads are a `std::map`
lookup. Synced writes are bound by one `F_FULLFSYNC` per write; group commit
(milestone 4) should amortize that across concurrent writers.
