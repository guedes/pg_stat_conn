# pg_stat_conn

`pg_stat_conn` is a lightweight extension for PostgreSQL 15+ that reduces the
database's blind spot regarding short-lived connections by tracking the
cumulative, real-time volume of connections, disconnections, and authentication
failures isolated per (database, user) pair. By exposing historical data and
allowing granular counter resets (much like `pg_stat_statements`), it provides a
great way to catch applications that abuse reconnections, validate connection
pooler efficiency, and identify intrusion attempts—all with minimal overhead and
no need to access server logs.

`pg_stat_conn` counts connections and disconnections per (database, user) pair,
tracks authentication failures, and records the timestamp of the last connection
and disconnection. Counters reset with `pg_stat_conn_reset()`, in the same
spirit as `pg_stat_statements_reset()`.

## Output example

```
psql -c "SELECT * FROM pg_stat_conn;"
```

```shell
     datname       │ usename  │ n_connections │ n_disconnections │ n_auth_failures │     last_connection_time      │    last_disconnection_time    
═══════════════════╪══════════╪═══════════════╪══════════════════╪═════════════════╪═══════════════════════════════╪═══════════════════════════════
 unkdown_db        │ guedes   │             1 │                1 │               0 │ 2026-07-15 19:25:14.695034-03 │ 2026-07-16 20:25:14.695209-03
 db_not_exists     │ test     │             1 │                1 │               0 │ 2026-07-15 19:47:19.78745-03  │ 2026-07-16 20:47:19.787564-03
 template1         │ postgres │             3 │                3 │               0 │ 2026-07-16 19:23:57.503401-03 │ 2026-07-16 19:23:57.504144-03
 postgres          │ test01   │             2 │                2 │               0 │ 2026-07-16 19:47:31.038608-03 │ 2026-07-16 19:47:44.52292-03
 test01            │ test01   │             1 │                1 │               0 │ 2026-07-16 19:47:24.470337-03 │ 2026-07-16 19:47:24.470512-03
 guedes            │ guedes   │         10848 │            10847 │               0 │ 2026-07-17 08:43:04.509131-03 │ 2026-07-17 08:36:43.231548-03
 postgres          │ postgres │             1 │                1 │               0 │ 2026-07-16 19:45:56.383174-03 │ 2026-07-16 19:46:23.261265-03
 test              │ guedes   │             2 │                2 │               0 │ 2026-07-16 19:44:55.753214-03 │ 2026-07-16 19:44:55.753404-03
 guedes            │ test01   │             1 │                1 │               4 │ 2026-07-16 19:47:44.520923-03 │ 2026-07-16 19:47:53.825463-03
```

Where:

- `datname`: the database name to which the connection belongs, even non‑existent databases appear here, useful for detecting connection attempts to unknown databases.
- `usename`: the username that established the connection, usernames that do not exist in the cluster can also be listed, helping spot unauthorized or mistyped login attempts.
- `n_connections`: total number of successful connection attempts recorded.
- `n_disconnections`: total number of recorded session terminations.
- `n_auth_failures`: count of authentication failures for that user/database.
- `last_connection_time`: timestamp of the most recent successful connection.
- `last_disconnection_time`: timestamp of the most recent recorded disconnection.


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
shared_preload_libraries = 'pg_stat_conn'
```

```
sudo systemctl restart postgresql   # or pg_ctl restart, depending on setup
```

Create the extension and generate a few connections:

```
psql -c "CREATE EXTENSION pg_stat_conn;"
psql -c "SELECT 1;"
psql -c "SELECT 1;"
```

Query the results:

```
psql -c "SELECT * FROM pg_stat_conn;"
```

```
 datname  | usename  | n_connections | n_disconnections  | n_auth_failures  |      last_connection_time     |    last_disconnection_time
----------+----------+---------------+-------------------+------------------+-------------------------------+--------------------------------
 postgres | postgres |             2 |                 2 |                0 | 2026-07-16 22:00:00.123456-03 | 2026-07-16 22:00:00.234567-03
```

The view and both functions are revoked from `PUBLIC`. Querying
`pg_stat_conn` requires a superuser or a role granted
`pg_read_all_stats`. Calling `pg_stat_conn_reset()` requires a superuser,
or a role that was explicitly granted `EXECUTE` on it.

Reset one pair or everything:

```
psql -c "SELECT pg_stat_conn_reset('postgres', 'postgres');"
psql -c "SELECT pg_stat_conn_reset();"
```

## Running the tests

The `t/` directory has TAP tests (`PostgreSQL::Test::Cluster`), the
same style used by `pg_stat_statements` and other modules that require
`shared_preload_libraries`: each test spins up its own temporary
cluster with `pg_stat_conn` preloaded, so a plain `pg_regress` run
against a general-purpose server cannot exercise it. They need a
PostgreSQL build with `--enable-tap-tests` (the case for most
distribution packages, including PGDG's).

```
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config install
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config installcheck
```

## Storage backend

Two backends are compiled in, selected automatically at build time:

- **PostgreSQL 15–17**: a private, fixed-size shared memory hash table.
  Counters are reset to zero on every server restart.
- **PostgreSQL 18+**: PostgreSQL 18's Cumulative Statistics System
  "custom stats kind" facility
  (https://wiki.postgresql.org/wiki/CustomCumulativeStats). The table
  grows dynamically (no fixed limit), and counters survive a clean
  server restart (not a crash).

  Custom stats kind IDs are a small, global, compile-time resource
  shared by every extension using this facility on a given server (9
  IDs total as of PG18). This build defaults to
  `PGSTAT_KIND_EXPERIMENTAL`, which risks colliding with another
  extension that does the same. If you reserve a permanent ID on the
  wiki page above, build with `-DPGSC_STATS_KIND=<reserved id>`.

## Configuration

`pg_stat_conn.max_entries` (integer, postmaster restart required,
default 128) sets how many (database, user) pairs are tracked on
**PostgreSQL 15–17 only**. Pairs beyond the limit are not counted. A
warning is logged the first time the limit is reached, and not
repeated after that. On PostgreSQL 18+ this setting does not exist;
the table has no fixed size.

## Benchmarks

`BENCHMARK.md` documents the methodology and results for measuring the
extension's overhead with `pgbench`, including a locking bug found and
fixed during testing. The scripts used to produce those numbers are in
`benchmarks/`.

# License

`pg_stat_conn` is released under the PostgreSQL License.

Copyright (c) 2026, Dickson S. Guedes

See [LICENSE](LICENSE) file for information.

