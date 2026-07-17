# pg_conn_stat

PostgreSQL extension that counts connections and disconnections per
(database, user) pair, tracks authentication failures, and records the
timestamp of the last connection and disconnection. Counters reset with
`pg_conn_stat_reset()`, in the same spirit as `pg_stat_statements_reset()`.

## Why track this

`pg_stat_activity` shows only the connections open right now.
`pg_stat_database` gives cumulative counters per database, not per
(database, user) pair, and has no record of failed authentication
attempts. Neither keeps a history of connect/disconnect events, and
neither lets you reset counters for one pair.

Opening and closing a connection has a cost: authentication runs a
password or SCRAM check, a backend process starts and later exits, and
shared memory structures are touched on both ends. `BENCHMARK.md` in
this repository measures 1 to 3 percent throughput cost when pgbench
opens a connection for every transaction, compared to reusing one
connection per client for the whole run. An application that
reconnects on every request pays that cost on every request, and
PostgreSQL does not expose how often that happens, or for which user
and database.

## Use cases

- Detect an application or service that opens a connection per request
  instead of reusing one from a pool. `n_connections` grows in step
  with request volume instead of leveling off.
- Flag authentication failures for a role without parsing the server
  log. `n_auth_failures` isolates this per (database, user) pair.
- Confirm that a connection pooler (PgBouncer or a pool built into the
  application) is reusing connections rather than opening one per
  transaction.
- Attribute connection volume to applications or service accounts when
  several share one cluster, without polling `pg_stat_activity` and
  losing everything that happened between polls.

## What it does not track

- Connections and disconnections are keyed by the database and user
  name supplied at connection time, not by their OID, because the OID
  is not yet resolved when `ClientAuthentication_hook` runs. Renaming a
  database or role starts a counter under the changed name, separate
  from the one under the old name.
- A backend that crashes instead of exiting through `proc_exit()`
  (killed process, server crash) is not counted as a disconnection.
- Background workers and autovacuum workers do not go through
  `ClientAuthentication_hook` and are not counted.

## Requirements

PostgreSQL 15 or later. Tested against PostgreSQL 18.4. Must be loaded
through `shared_preload_libraries`.

## Install and test

Build and install with PGXS:

```
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config
sudo make USE_PGXS=1 PG_CONFIG=/path/to/pg_config install
```

Add the extension to `shared_preload_libraries` in `postgresql.conf` and
restart the server:

```
shared_preload_libraries = 'pg_conn_stat'
```

```
sudo systemctl restart postgresql   # or pg_ctl restart, depending on setup
```

Create the extension and generate a few connections:

```
psql -c "CREATE EXTENSION pg_conn_stat;"
psql -c "SELECT 1;"
psql -c "SELECT 1;"
```

Query the results:

```
psql -c "SELECT * FROM pg_conn_stat;"
```

```
 datname  | usename  | n_connections | n_disconnections  | n_auth_failures  |      last_connection_time     |    last_disconnection_time
----------+----------+---------------+-------------------+------------------+-------------------------------+--------------------------------
 postgres | postgres |             2 |                 2 |                0 | 2026-07-16 22:00:00.123456-03 | 2026-07-16 22:00:00.234567-03
```

The view and both functions are revoked from `PUBLIC`. Querying
`pg_conn_stat` requires a superuser or a role granted
`pg_read_all_stats`. Calling `pg_conn_stat_reset()` requires a superuser,
or a role that was explicitly granted `EXECUTE` on it.

Reset one pair or everything:

```
psql -c "SELECT pg_conn_stat_reset('postgres', 'postgres');"
psql -c "SELECT pg_conn_stat_reset();"
```

## Configuration

`pg_conn_stat.max_entries` (integer, postmaster restart required,
default 128) sets how many (database, user) pairs are tracked. Pairs
beyond the limit are not counted. A warning is logged the first time
the limit is reached, and not repeated after that.

## Benchmarks

`BENCHMARK.md` documents the methodology and results for measuring the
extension's overhead with `pgbench`, including a locking bug found and
fixed during testing. The scripts used to produce those numbers are in
`benchmarks/`.

# License

`pg_conn_stat` is released under the PostgreSQL License.

Copyright (c) 2026, Dickson S. Guedes

See [LICENSE](LICENSE) file for information.

