# Copyright (c) 2026, Dickson S. Guedes

# Basic pg_stat_conn coverage: extension setup, default privileges on
# the view and the reset function, connection/disconnection counters
# (including attempts against a database that does not exist), and
# pg_stat_conn_reset() with every combination of arguments.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'pg_stat_conn'");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_stat_conn;');

my $superuser = $node->safe_psql('postgres', 'SELECT current_user;');

# ---------------------------------------------------------------------
# Catalog shape and default privileges.
# ---------------------------------------------------------------------

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_catalog.pg_views WHERE viewname = 'pg_stat_conn'"
	),
	'1',
	'pg_stat_conn view exists');

is( $node->safe_psql(
		'postgres',
		"SELECT array_agg(attname::text ORDER BY attnum) FROM pg_attribute"
		  . " WHERE attrelid = 'pg_stat_conn'::regclass AND attnum > 0"
	),
	'{datname,usename,n_connections,n_disconnections,n_auth_failures,last_connection_time,last_disconnection_time}',
	'pg_stat_conn view has the documented columns in order');

is( $node->safe_psql('postgres',
		"SELECT has_table_privilege('public', 'pg_stat_conn', 'SELECT')"),
	'f',
	'pg_stat_conn view is not readable by PUBLIC');

is( $node->safe_psql(
		'postgres',
		"SELECT has_function_privilege('public', 'pg_stat_conn()', 'EXECUTE')"
	),
	'f',
	'pg_stat_conn() function is revoked from PUBLIC');

is( $node->safe_psql(
		'postgres',
		"SELECT has_function_privilege('public', 'pg_stat_conn_reset(text,text)', 'EXECUTE')"
	),
	'f',
	'pg_stat_conn_reset() function is revoked from PUBLIC');

# A role with none of the above privileges can neither query the view
# nor call the reset function.
$node->safe_psql('postgres', 'CREATE ROLE conn_stat_noaccess LOGIN;');

my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	'SELECT * FROM pg_stat_conn;',
	connstr => $node->connstr('postgres') . " user=conn_stat_noaccess");
isnt($ret, 0, 'unprivileged role cannot SELECT from pg_stat_conn');
like($stderr, qr/permission denied/, 'error is permission denied');

($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	'SELECT pg_stat_conn_reset();',
	connstr => $node->connstr('postgres') . " user=conn_stat_noaccess");
isnt($ret, 0, 'unprivileged role cannot call pg_stat_conn_reset()');
like($stderr, qr/permission denied/, 'error is permission denied');

# Granting pg_read_all_stats and EXECUTE lifts those restrictions, as
# documented in the README.
$node->safe_psql('postgres',
	'GRANT pg_read_all_stats TO conn_stat_noaccess;');
($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	'SELECT * FROM pg_stat_conn;',
	connstr => $node->connstr('postgres') . " user=conn_stat_noaccess");
is($ret, 0, 'role granted pg_read_all_stats can SELECT from pg_stat_conn');

$node->safe_psql('postgres',
	'GRANT EXECUTE ON FUNCTION pg_stat_conn_reset(text, text) TO conn_stat_noaccess;'
);
($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	'SELECT pg_stat_conn_reset();',
	connstr => $node->connstr('postgres') . " user=conn_stat_noaccess");
is($ret, 0,
	'role granted EXECUTE can call pg_stat_conn_reset()');

# ---------------------------------------------------------------------
# Connection / disconnection counters.
# ---------------------------------------------------------------------

$node->safe_psql('postgres', 'CREATE ROLE conn_stat_user LOGIN;');

# Clear out every entry accumulated by the setup above, through a
# role that is *not* one of the pairs counted below. Whichever
# session issues a reset always re-creates its own (database, user)
# entry when it disconnects afterward (see pgsc_maybe_init_identity()
# in pg_stat_conn.c), so resetting through conn_stat_user or the
# bootstrap superuser would leave an extra row behind.
$node->psql(
	'postgres',
	'SELECT pg_stat_conn_reset();',
	connstr => $node->connstr('postgres') . " user=conn_stat_noaccess");

# Each connect_ok()/connect_fails() call is one psql invocation: one
# connection opened and closed, on its own.
$node->connect_ok($node->connstr('postgres') . " user=$superuser",
	"connect as postgres/$superuser (1)");
$node->connect_ok($node->connstr('postgres') . " user=$superuser",
	"connect as postgres/$superuser (2)");
$node->connect_ok($node->connstr('postgres') . " user=$superuser",
	"connect as postgres/$superuser (3)");
$node->connect_ok($node->connstr('postgres') . " user=conn_stat_user",
	'connect as postgres/conn_stat_user');

# A database that does not exist is still tracked, under the raw name
# supplied by the client.
$node->connect_fails(
	$node->connstr('no_such_database') . " user=$superuser",
	'connecting to a non-existent database fails',
	expected_stderr => qr/database "no_such_database" does not exist/);

($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	"SELECT datname, usename, n_connections, n_disconnections, n_auth_failures"
	  . " FROM pg_stat_conn WHERE usename <> 'conn_stat_noaccess'"
	  . " ORDER BY datname, usename",
	connstr => $node->connstr('postgres') . " user=conn_stat_noaccess");
is( $stdout,
	join("\n",
		"no_such_database|$superuser|1|1|0",
		"postgres|conn_stat_user|1|1|0",
		"postgres|$superuser|3|3|0"),
	'connections, disconnections, and unknown databases are tracked per (database, user)'
);

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_conn"
		  . " WHERE (last_connection_time IS NULL OR last_disconnection_time IS NULL)"
		  . " AND usename <> 'conn_stat_noaccess'"
	),
	'0',
	'last_connection_time and last_disconnection_time are populated for every pair'
);

# ---------------------------------------------------------------------
# pg_stat_conn_reset().
# ---------------------------------------------------------------------

# Reset one specific pair: only that row disappears, leaving the
# caller's own (postgres, $superuser) entry untouched.
$node->safe_psql('postgres',
	"SELECT pg_stat_conn_reset('postgres', 'conn_stat_user');");
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_stat_conn"
		  . " WHERE datname = 'postgres' AND usename = 'conn_stat_user'"),
	'0',
	'pg_stat_conn_reset(datname, usename) removes only the matching pair');
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_stat_conn WHERE (datname, usename) IN"
		  . " (('no_such_database', '$superuser'), ('postgres', '$superuser'))"
	),
	'2',
	'the other pairs survive a targeted reset');

# Reset by database name only: every user under that database goes
# away, other databases are untouched.
$node->safe_psql('postgres',
	"SELECT pg_stat_conn_reset('no_such_database', NULL);");
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_stat_conn WHERE datname = 'no_such_database'"),
	'0',
	'pg_stat_conn_reset(datname, NULL) clears every user for that database');
is( $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_stat_conn"
		  . " WHERE datname = 'postgres' AND usename = '$superuser'"),
	'1',
	'pairs under other databases survive');

# Reset by user name only: every database for that user goes away.
# This particular reset deletes the calling session's own identity,
# so the check has to run in the same psql invocation as the reset --
# otherwise the session's own disconnect, right after, would recreate
# the very row just removed (again, pgsc_maybe_init_identity()).
$node->connect_ok($node->connstr('postgres') . " user=$superuser",
	're-connect as postgres/' . $superuser);
is( $node->safe_psql(
		'postgres',
		"SELECT pg_stat_conn_reset(NULL, '$superuser') \\gset\n"
		  . "SELECT count(*) FROM pg_stat_conn WHERE usename = '$superuser';"
	),
	'0',
	'pg_stat_conn_reset(NULL, usename) clears every database for that user');

# A full reset clears everything that is left, checked the same way.
is( $node->safe_psql(
		'postgres',
		"SELECT pg_stat_conn_reset() \\gset\n"
		  . 'SELECT count(*) FROM pg_stat_conn;'
	),
	'0',
	'pg_stat_conn_reset() with no arguments clears every pair');

$node->stop;

done_testing();
