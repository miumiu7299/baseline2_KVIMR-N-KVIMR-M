# Baseline2.2 — KVIMR-N / KVIMR-M

Prototype implementation and evaluation scripts for the Baseline2.2 raw-middleware KVIMR design.

## Architecture

```text
YCSB
  ↓
RocksDB 5.11.3 + KVIMR modifications
  ↓
KVIMR Middleware
  ├─ SST level detection
  ├─ S2TMap
  ├─ Level-aware track placement
  └─ Naive / Merged RMW
  ↓
raw dm-imrsim
  ↓
Backing Device
```

The SST payload path bypasses ext4 and accesses the raw IMRSim mapper. WAL/MANIFEST and other normal RocksDB metadata remain on the regular filesystem DB directory.

## Modes

- **KVIMR-N**: Naive track-level RMW (`kvimr.rmw_mode=naive`)
- **KVIMR-M**: Merged RMW (`kvimr.rmw_mode=merged`)

The merged mode combines compatible updates to reduce repeated backup/restore work on affected TOP tracks.

## Evaluation

The formal comparison uses YCSB workloads A–F with:

```text
RocksDB         5.11.3 + KVIMR modifications
Records         300,000
Operations      1,000,000
Threads         8
Field count     10
Field length    100 bytes
Repetitions     3
Zones           79
Mapper I/O cap  4 KiB
```

See [TESTING.md](TESTING.md) for the complete build, safety, smoke-test, formal A–F ×3, validation, and result-analysis procedure.

## Important safety note

The formal runner resets the backing device before each repetition.

**Never use the system disk as the backing device.** In the current VM, `/dev/sda` is the system disk and must not be used for destructive IMRSim tests.

## Result files

Each formal run produces:

```text
runs.csv
medians.csv
comparison.md
manifest.txt
workload*/run-*/...
```

Per-run artifacts include YCSB logs, IMRSim/debug counters, device statistics, mapper configuration, dmesg snapshots, and relevant RocksDB metadata.

## Status

Baseline2.2 currently includes:

- level-aware placement
- S2TMap
- KVIMR-N Naive RMW
- KVIMR-M Merged RMW
- YCSB A–F automated evaluation
- RMW/merge runtime counters
- I/O accounting and result export

Baseline2.2 is the **Raw Middleware** prototype. It should be distinguished from the separate ext4 Common-Stack Baseline2.1 path.
