# Copyright (c) 2026, Dickson S. Guedes

# pg_stat_conn() and pg_stat_conn_reset() must refuse to run if
# pg_stat_conn was not loaded through shared_preload_libraries.
# CREATE EXTENSION itself does not load the library (functions are
# resolved lazily on first call), so it succeeds either way; only
# calling the functions exposes the missing storage backend.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_stat_conn;');

my ($ret, $stdout, $stderr) =
  $node->psql('postgres', 'SELECT * FROM pg_stat_conn;');
isnt($ret, 0,
	'pg_stat_conn() fails when pg_stat_conn is not in shared_preload_libraries'
);
like(
	$stderr,
	qr/pg_stat_conn must be loaded via "shared_preload_libraries"/,
	'error names shared_preload_libraries');

($ret, $stdout, $stderr) =
  $node->psql('postgres', 'SELECT pg_stat_conn_reset();');
isnt($ret, 0,
	'pg_stat_conn_reset() fails the same way when not preloaded');
like(
	$stderr,
	qr/pg_stat_conn must be loaded via "shared_preload_libraries"/,
	'error names shared_preload_libraries');

$node->stop;

done_testing();
