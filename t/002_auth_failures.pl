# Copyright (c) 2026, Dickson S. Guedes

# pg_stat_conn.n_auth_failures coverage: a wrong password against a
# role that exists, and any password against a role name that does
# not exist at all. Both must be counted as failures, and neither
# must bump n_connections -- per the comment in pg_stat_conn.c
# explaining that failed-authentication attempts may not even
# correspond to a real database/role.

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
$node->safe_psql('postgres',
	"CREATE ROLE conn_stat_pwuser LOGIN PASSWORD 'right-password';");

# Force password authentication for these two role names only, ahead
# of the "local all all trust" line initdb put in place, so every
# other local connection (including the ones safe_psql() makes as the
# bootstrap superuser) is unaffected.
my $hba_file = $node->data_dir . '/pg_hba.conf';
my $orig_hba = PostgreSQL::Test::Utils::slurp_file($hba_file);
open my $fh, '>', $hba_file
  or die "could not open \"$hba_file\": $!";
print $fh "local all conn_stat_pwuser scram-sha-256\n";
print $fh "local all conn_stat_ghost scram-sha-256\n";
print $fh $orig_hba;
close $fh;
$node->reload;

my $superuser = $node->safe_psql('postgres', 'SELECT current_user;');
$node->safe_psql('postgres', 'SELECT pg_stat_conn_reset();');

# Wrong password for a role that does exist: an auth failure, no
# connection.
$node->connect_fails(
	$node->connstr('postgres') . ' user=conn_stat_pwuser password=wrong-password',
	'wrong password is rejected',
	expected_stderr => qr/password authentication failed/);

# Right password: a normal connection (and disconnection).
$node->connect_ok(
	$node->connstr('postgres') . ' user=conn_stat_pwuser password=right-password',
	'correct password is accepted');

# A role name that was never created: still an auth failure, tracked
# under the name the client asked for.
$node->connect_fails(
	$node->connstr('postgres') . ' user=conn_stat_ghost password=anything',
	'a non-existent role fails authentication too',
	expected_stderr => qr/password authentication failed/);

is( $node->safe_psql(
		'postgres',
		"SELECT datname, usename, n_connections, n_disconnections, n_auth_failures"
		  . " FROM pg_stat_conn WHERE usename <> '$superuser' ORDER BY usename"
	),
	join("\n",
		"postgres|conn_stat_ghost|0|0|1",
		"postgres|conn_stat_pwuser|1|1|1"),
	'wrong password and unknown role both count as auth failures, without counting as connections'
);

$node->stop;

done_testing();
