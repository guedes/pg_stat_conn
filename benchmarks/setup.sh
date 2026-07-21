#!/usr/bin/env bash
# Creates a disposable PostgreSQL cluster (outside /var/lib/postgresql, no
# root needed) and initializes the pgbench schema on it. Run once before
# run_bench.sh; can be re-run to recreate from scratch (delete
# benchmarks/pgdata first).
#
# Accepted environment variables (all optional):
#   PG_CONFIG   path to the pg_config to use (default: pg_config on PATH)
#   PGPORT      port for the test cluster (default: 5599)
#   SCALE       pgbench -i scale factor (default: 10, ~1M rows)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXT_SRC_DIR="$(dirname "$SCRIPT_DIR")"

PG_CONFIG=${PG_CONFIG:-pg_config}
PG_BINDIR=$("$PG_CONFIG" --bindir)
PGDATA="$SCRIPT_DIR/pgdata"
PGPORT=${PGPORT:-5599}
EXTSTAGE="$SCRIPT_DIR/extstage"
SCALE=${SCALE:-10}

if [ -d "$PGDATA" ]; then
	echo "==> $PGDATA already exists, skipping initdb (delete the directory to start over)"
else
	echo "==> initdb in $PGDATA"
	"$PG_BINDIR/initdb" -D "$PGDATA" -U postgres --no-sync >/dev/null
fi

echo "==> building the extension"
make -C "$EXT_SRC_DIR" USE_PGXS=1 PG_CONFIG="$PG_CONFIG" >/dev/null

echo "==> staging extension_control_path/dynamic_library_path for testing"
mkdir -p "$EXTSTAGE/extension"
cp "$EXT_SRC_DIR/pg_stat_conn.so" "$EXTSTAGE/"
sed "s|module_pathname = '\$libdir/pg_stat_conn'|module_pathname = 'pg_stat_conn'|" \
	"$EXT_SRC_DIR/pg_stat_conn.control" >"$EXTSTAGE/extension/pg_stat_conn.control"
cp "$EXT_SRC_DIR/pg_stat_conn--1.0.sql" "$EXTSTAGE/extension/"

echo "==> configuring postgresql.conf / pg_hba.conf"
CONF="$PGDATA/postgresql.conf"
sed -i '/# pg_stat_conn benchmark overrides/,$d' "$CONF"
cat >>"$CONF" <<EOF
# pg_stat_conn benchmark overrides
port = $PGPORT
listen_addresses = 'localhost'
unix_socket_directories = '$SCRIPT_DIR'
dynamic_library_path = '$EXTSTAGE'
extension_control_path = '$EXTSTAGE:\$system'
shared_preload_libraries = ''
EOF

cat >"$PGDATA/pg_hba.conf" <<EOF
local   all             all                                     trust
host    all             all             127.0.0.1/32            trust
host    all             all             ::1/128                 trust
EOF

echo "==> starting the cluster to initialize the pgbench schema"
"$PG_BINDIR/pg_ctl" -D "$PGDATA" -l "$SCRIPT_DIR/server.log" -w start

"$PG_BINDIR/createdb" -h "$SCRIPT_DIR" -p "$PGPORT" -U postgres bench 2>/dev/null || true
"$PG_BINDIR/pgbench" -h "$SCRIPT_DIR" -p "$PGPORT" -U postgres -i -s "$SCALE" -q bench

"$PG_BINDIR/pg_ctl" -D "$PGDATA" stop -m fast >/dev/null

echo "==> setup complete. Run ./run_bench.sh next."
