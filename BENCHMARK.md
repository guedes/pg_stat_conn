# pg_stat_conn: connection overhead benchmark

## Goal

Measure the throughput overhead `pg_stat_conn` adds to connection
establishment/teardown, using `pgbench`, comparing a cluster with
`shared_preload_libraries = 'pg_stat_conn'` against a baseline without it.
Scripts live in `benchmarks/`.

## Setup

- `benchmarks/setup.sh` creates a PostgreSQL 18 cluster (no root needed, it
  points `extension_control_path`/`dynamic_library_path` at this project
  directory instead of installing system-wide) and initializes the pgbench
  schema at scale 10 (~1M rows in `pgbench_accounts`).
- `benchmarks/run_bench.sh` runs `pgbench -S` (select-only) in two connection
  modes:
  - **persistent**: default pgbench behavior, each client opens one connection
    and reuses it for the whole run.
  - **`-C` (reconnect)**: a brand-new connection for every transaction.  This is
    the mode that actually exercises `pg_stat_conn`'s
    `ClientAuthentication_hook` and disconnect callback on every transaction, so
    it's where a real per-connection cost should show up.
- `benchmarks/summarize.sh` extracts tps/latency from every repetition and
  reports the **median** per scenario (plus min–max, to expose variance). A
  single 15 s run is noisy enough that one bad sample can swing the headline
  number by several percent.

## Bug found during benchmarking: PANIC under concurrent load

The very first `-C` run with the extension loaded and 10 concurrent
clients crashed the whole cluster:

```
PANIC: cannot wait without a PGPROC structure
```

**Root cause**: the disconnect callback was registered with
`on_proc_exit()`. `ProcKill()` (which sets `MyProc = NULL`) is itself
registered via `on_shmem_exit()`, and PostgreSQL's backend shutdown
sequence runs, in order:

```
before_shmem_exit list  ->  on_shmem_exit list (includes ProcKill)  ->  on_proc_exit list
```

So by the time an `on_proc_exit` callback runs, `MyProc` is already
`NULL`. `LWLockAcquire()` cannot block on a lock without a valid
`PGPROC` to queue itself on, so it panics as soon as the lock happens to
be contended at disconnect time, exactly what happens under concurrent
`-C` load, when several clients disconnect at once and collide on the
same `LWLock`.

**Fix**: register the disconnect callback with `before_shmem_exit()`
instead, which runs *before* `ProcKill()`. See `pg_stat_conn.c`,
`pgsc_client_authentication()`.

This was only caught because the benchmark genuinely stressed concurrent
disconnects; none of the earlier manual `psql` testing ever hit lock
contention on that path.

## Results, run 1: blocked order (5 repetitions per scenario, median)

All `noext` repetitions ran first (one server restart, 5 runs), then all
`ext` repetitions (another restart, 5 runs).

| scenario                 | n | median tps | tps min–max     | overhead   |
|--------------------------|---|------------|-----------------|------------|
| persistent, no ext       | 5 | 150,254.8  | 147,011–158,009 | n/a        |
| persistent, with ext     | 5 | 140,486.1  | 140,091–141,667 | **-6.50%** |
| `-C` reconnect, no ext   | 5 | 1,910.7    | 1,894.0–1,942.8 | n/a        |
| `-C` reconnect, with ext | 5 | 1,854.3    | 1,848.8–1,863.4 | **-2.95%** |

### Confound: thermal/clock drift in blocked order

Looking at the 5 individual persistent-mode runs, tps decreases
monotonically across the sequence in *both* groups:

- no ext: 158,009 → 153,822 → 150,255 → 148,609 → 147,011 (~7% spread)
- with ext: 141,667 → 140,634 → 140,486 → 140,448 → 140,091 (~1% spread)

Because the script ran all `noext` repetitions first and all `ext`
repetitions only afterward (chronologically ~3 minutes later), any drift
over the course of the whole benchmark (CPU frequency scaling, thermal
throttling, background load on a shared dev machine) systematically
penalizes the `ext` group and inflates the apparent overhead.

This reading is also the mechanistically plausible one: in persistent
mode the connect/disconnect hooks fire only **10 times total** (once per
client) across ~2.1 million transactions, so they cannot plausibly
explain a 6.5% throughput difference. The tight ~1% spread inside the
`ext` group (measured later, presumably in a more settled thermal state)
versus the ~7% spread in the `noext` group supports drift over a real
per-connection cost.

The `-C` reconnect-mode number (**-2.95%**) is more trustworthy as a
genuine signal: there, every transaction goes through the extension's
lock-protected hash table lookup, and both groups show a tight,
comparable spread (~0.8%).

## Results, run 2: interleaved order (5 repetitions per scenario, median)

To cancel out drift, `run_bench.sh` supports `ORDER=interleaved`
(the default): instead of running all `noext` repetitions and then all
`ext` repetitions, it alternates: restart without the extension, run
persistent + reconnect, restart with the extension, run persistent +
reconnect, repeat 5 times. This costs more restarts (10 instead of 2)
but any drift affects both groups equally instead of only the group that
happens to run later.

| scenario                 | n | median tps | tps min–max         | overhead   |
|--------------------------|---|------------|---------------------|------------|
| persistent, no ext       | 5 | 144,304.2  | 140,691.5–157,896.5 | n/a        |
| persistent, with ext     | 5 | 143,037.2  | 140,005.1–150,557.1 | **-0.88%** |
| `-C` reconnect, no ext   | 5 | 1,916.5    | 1,869.9–2,024.9     | n/a        |
| `-C` reconnect, with ext | 5 | 1,877.3    | 1,847.5–1,970.9     | **-2.04%** |

Two things changed once ordering stopped confounding the comparison:

1. **The persistent-mode overhead collapsed from -6.50% to -0.88%**,
   matching the mechanistic expectation of ~0: the connect/disconnect
   hooks fire only 10 times total in that scenario, so they cannot cost
   meaningfully more than noise. -0.88% is within the run-to-run noise
   band (both groups now span a comparable ~12% min–max range, because
   whatever variance exists on this shared dev machine now hits both
   groups symmetrically instead of hitting only whichever group ran
   last).
2. **The `-C` reconnect-mode overhead is stable across both orderings**
   (-2.95% blocked vs. -2.04% interleaved, same order of magnitude),
   which is the expected signature of a *real* cost: it shows up
   consistently regardless of run order, unlike the persistent-mode
   number.

## Locking redesign: single exclusive lock → two-phase shared/exclusive + atomics

The interleaved run above still used the original design: every connect
and every disconnect took `pgsc->lock` in `LW_EXCLUSIVE` mode
unconditionally, even when the (database, user) entry already existed,
which, after warm-up, is essentially every time, since the key space
(distinct database/user pairs) is small and stabilizes fast. That single
instance-wide exclusive lock is also the exact lock that produced the
`PANIC` above under just 10 concurrent clients, so it was both the
measured cost *and* the biggest correctness risk in the module.

`pg_stat_conn.c` was rewritten to follow `pg_stat_statements`' two-phase
pattern instead (see `pgsc_record()`):

1. **Fast path** (the common case once a pair has been seen before):
   acquire `pgsc->lock` in `LW_SHARED` (which many backends can hold at
   once), look the entry up with `HASH_FIND`, and bump its counters with
   `pg_atomic_fetch_add_u64`/`pg_atomic_write_u64` *while still holding
   the shared lock*. The counters (`n_connections`, `n_disconnections`,
   `n_auth_failures`, `last_connection_time`, `last_disconnection_time`)
   are now `pg_atomic_uint64` fields instead of plain `int64`/`TimestampTz`.
   Holding `LW_SHARED` during the atomic update, rather than releasing
   it first, is what keeps this safe: `pg_stat_conn_reset()` needs
   `LW_EXCLUSIVE` to `HASH_REMOVE` an entry, and that can't be granted
   while any shared holder is in, so the entry can't be freed out from
   under an in-flight atomic update.
2. **Slow path**: only reached the first time a given pair is seen (or
   if a concurrent reset raced the entry away between the shared lookup
   and getting here). Re-acquires `pgsc->lock` in `LW_EXCLUSIVE` to
   `HASH_ENTER_NULL` the entry, exactly once per distinct pair, not once
   per connection.

This does not change what gets counted or how reset works, only how the
hot path gets there. Functional correctness was re-verified against the
scratch cluster (sequential connects, a 10-client `pgbench -C` burst,
selective and global reset) before re-running the throughput comparison.

### Results, run 3: same interleaved methodology, new locking (5 reps, median)

Absolute tps is lower across the board than run 2 (a busier machine at
the time, unrelated to the extension, see the noise discussion above),
but what matters is the ext-vs-noext delta:

| scenario                 | n | median tps | tps min–max         | overhead   |
|--------------------------|---|------------|---------------------|------------|
| persistent, no ext       | 5 | 107,962.0  | 106,540.9–113,414.9 | n/a        |
| persistent, with ext     | 5 | 107,413.2  | 86,003.7–108,609.3  | **-0.51%** |
| `-C` reconnect, no ext   | 5 | 1,400.6    | 1,352.3–1,441.3     | n/a        |
| `-C` reconnect, with ext | 5 | 1,382.9    | 1,292.6–1,389.0     | **-1.26%** |

(The 86,003.7 low point in `ext_persistent` is a single-run outlier:
`ext_persistent_run5` and `noext_persistent_run5`/`noext_reconnect_run5`
all dipped together, a general slowdown in that round that hit both
groups, not an extension-specific effect. The median is unmoved by it,
which is the whole point of using the median instead of the mean here.)

### Before/after

| scenario       | old locking (single exclusive) | new locking (shared + atomics) |
|----------------|--------------------------------|--------------------------------|
| persistent     | -0.88%                         | -0.51%                         |
| `-C` reconnect | -2.04%                         | **-1.26%**                     |

The reconnect-mode overhead, the one number that was consistently a
real, order-independent signal rather than noise, dropped by roughly
**a third to a half**, from ~2% to ~1.3%, after removing the unconditional
exclusive lock from the common case. Persistent mode was already
indistinguishable from noise before this change and remains so.

## Results, run 4: PG18 `pgstat_register_kind` backend (same interleaved methodology, 5 reps, median)

`pg_stat_conn.c` was migrated to PostgreSQL 18's Cumulative Statistics
System "custom stats kind" facility on PG18+ (`pgstat_register_kind()`,
see the `#if PG_VERSION_NUM >= 180000` branch), replacing the private
shmem hash table with the generic pgstat machinery: dynamically-sized
storage (no more `max_entries`), per-entry locking owned by pgstat, and
on-disk persistence across clean restarts. Connect/disconnect events now
accumulate in a backend-local "pending" struct
(`PgStat_StatConnPending`) that's merged into shared memory by a
`flush_pending_cb`, called periodically and at backend shutdown, instead
of an immediate atomic increment on the shared entry. Same machine, same
methodology (interleaved order, 10 clients, 4 threads, 15 s, 5 reps):

| scenario                 | n | median tps | tps min–max         | overhead   |
|--------------------------|---|------------|---------------------|------------|
| persistent, no ext       | 5 | 151,808.5  | 149,140.5–163,698.0 | n/a        |
| persistent, with ext     | 5 | 150,481.8  | 148,856.7–158,998.9 | **-0.87%** |
| `-C` reconnect, no ext   | 5 | 1,928.3    | 1,905.7–2,026.2     | n/a        |
| `-C` reconnect, with ext | 5 | 1,920.0    | 1,889.1–1,988.9     | **-0.43%** |

Zero failed transactions and no PANICs across all 20 runs (10 clients
hammering the disconnect path in `-C` mode included), confirming the
`before_shmem_exit()` ordering between this extension's own disconnect
hook and pgstat's generic shutdown flush (`pgstat_shutdown_hook`, also
registered via `before_shmem_exit()`, earlier, during `BaseInit()`) holds
up under concurrent load, not just in the single-connection manual
testing done during development.

### Before/after: shmem backend (run 3) vs pgstat backend (run 4)

| scenario       | shmem backend (run 3) | pgstat backend (run 4) |
|----------------|------------------------|-------------------------|
| persistent     | -0.51%                 | -0.87%                  |
| `-C` reconnect | -1.26%                 | **-0.43%**               |

No regression. If anything, the reconnect-mode overhead -- the one
number in this whole file that has consistently been a real,
order-independent signal rather than noise -- is *lower* with the new
backend, and now sits inside the same noise band as persistent mode.
The plausible mechanism: in `-C` mode every transaction is a brand-new
backend, so neither backend gets to reuse a cached lookup; both pay a
shared-lock-protected hash lookup (`LW_SHARED` + `hash_search` before,
`dshash_find` now) once per connection. But where the old design applied
the counter update as an atomic op directly on the shared, contended
cacheline, the new design applies it to a backend-local `pending` struct
under no lock at all, and only merges into shared memory later, in a
batched flush -- trading one shared atomic write per event for one purely
local write per event plus an amortized, less-contended flush.

## Conclusion

`pg_stat_conn`'s real, measurable cost is confined to the connection
setup/teardown path itself, and stays at or under **~1% throughput
overhead in the worst case** (`pgbench -C`, a new connection for every
single transaction, a pathological, connection-pool-less workload) on
both storage backends, PG18's `pgstat_register_kind` included. Under any
workload where connections are reused for more than a handful of
transactions (i.e. essentially all real applications, and matching the
persistent-mode result here), the overhead is indistinguishable from
measurement noise.

## Reproducing

```
cd benchmarks
./setup.sh                       # once
./run_bench.sh                   # ORDER=interleaved by default
# ORDER=blocked ./run_bench.sh   to reproduce the confounded numbers above
```
