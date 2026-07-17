#!/usr/bin/env bash
# Compares pg_conn_stat's connection overhead by running pgbench twice
# (with and without "shared_preload_libraries = pg_conn_stat"), each time
# in two modes:
#   - persistent: connections opened once and reused (pgbench default)
#   - -C (reconnect): a brand-new connection for every transaction. This is
#     where the extension's overhead, if any, should show up.
# Each scenario runs REPEATS times; summarize.sh takes the median across
# runs. See BENCHMARK.md for why ORDER=interleaved is the default: running
# all "noext" repetitions first and only then all "ext" repetitions
# (ORDER=blocked) let thermal/clock drift over the course of the benchmark
# systematically penalize whichever group ran last.
# Requires setup.sh to have been run first.
#
# Accepted environment variables (all optional):
#   PG_CONFIG   path to the pg_config to use (default: pg_config on PATH)
#   PGPORT      port for the test cluster (default: 5599)
#   CLIENTS     concurrent pgbench clients (default: 10)
#   THREADS     pgbench threads (default: 4)
#   DURATION    duration of each run in seconds (default: 15)
#   REPEATS     repetitions per scenario, for the median (default: 5)
#   ORDER       'interleaved' (default) alternates noext/ext every
#               repetition (more restarts, cancels out drift); 'blocked'
#               runs all repetitions of one side, then all of the other
#               (fewer restarts, more sensitive to drift over time).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PG_CONFIG=${PG_CONFIG:-pg_config}
PG_BINDIR=$("$PG_CONFIG" --bindir)
PGDATA="$SCRIPT_DIR/pgdata"
PGPORT=${PGPORT:-5599}
RESULTS="$SCRIPT_DIR/results"
CLIENTS=${CLIENTS:-10}
THREADS=${THREADS:-4}
DURATION=${DURATION:-15}
REPEATS=${REPEATS:-5}
ORDER=${ORDER:-interleaved}

if [ ! -d "$PGDATA" ]; then
	echo "PGDATA ($PGDATA) does not exist. Run ./setup.sh first." >&2
	exit 1
fi

mkdir -p "$RESULTS"
rm -f "$RESULTS"/*_run*.txt

set_preload() {
	sed -i "s|^shared_preload_libraries = .*|shared_preload_libraries = '$1'|" "$PGDATA/postgresql.conf"
}

restart_pg() {
	"$PG_BINDIR/pg_ctl" -D "$PGDATA" -l "$SCRIPT_DIR/server.log" -w restart >/dev/null 2>&1 \
		|| "$PG_BINDIR/pg_ctl" -D "$PGDATA" -l "$SCRIPT_DIR/server.log" -w start >/dev/null
}

run_once() {
	local label="$1" idx="$2"
	shift 2
	echo "==> pgbench $label run $idx/$REPEATS (clients=$CLIENTS threads=$THREADS duration=${DURATION}s)"
	"$PG_BINDIR/pgbench" -h "$SCRIPT_DIR" -p "$PGPORT" -U postgres \
		-c "$CLIENTS" -j "$THREADS" -T "$DURATION" -S -P 5 "$@" bench \
		| tee "$RESULTS/${label}_run${idx}.txt"
	echo
}

switch_mode() {
	local mode="$1"
	if [ "$mode" = "noext" ]; then
		echo "### Without pg_conn_stat ###"
		set_preload ""
	else
		echo "### With pg_conn_stat (shared_preload_libraries) ###"
		set_preload "pg_conn_stat"
	fi
	restart_pg
	sleep 1
}

if [ "$ORDER" = "interleaved" ]; then
	for i in $(seq 1 "$REPEATS"); do
		for MODE in noext ext; do
			switch_mode "$MODE"
			run_once "${MODE}_persistent" "$i"
			run_once "${MODE}_reconnect" "$i" -C
		done
	done
elif [ "$ORDER" = "blocked" ]; then
	for MODE in noext ext; do
		switch_mode "$MODE"
		for i in $(seq 1 "$REPEATS"); do
			run_once "${MODE}_persistent" "$i"
			run_once "${MODE}_reconnect" "$i" -C
		done
	done
else
	echo "Invalid ORDER: $ORDER (use 'interleaved' or 'blocked')" >&2
	exit 1
fi

"$PG_BINDIR/pg_ctl" -D "$PGDATA" stop -m fast >/dev/null

"$SCRIPT_DIR/summarize.sh"
