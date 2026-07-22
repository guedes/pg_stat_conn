-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_stat_conn" to load this file. \quit

CREATE FUNCTION pg_stat_conn(
    OUT datname text,
    OUT usename text,
    OUT n_connections bigint,
    OUT n_disconnections bigint,
    OUT n_auth_failures bigint,
    OUT last_connection_time timestamptz,
    OUT last_disconnection_time timestamptz
) RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C VOLATILE PARALLEL SAFE ROWS 100;

REVOKE ALL ON FUNCTION pg_stat_conn() FROM PUBLIC;

CREATE VIEW pg_stat_conn AS
    SELECT * FROM pg_stat_conn();

GRANT EXECUTE ON FUNCTION pg_stat_conn() TO pg_read_all_stats;
GRANT SELECT ON pg_stat_conn TO pg_read_all_stats;

CREATE FUNCTION pg_stat_conn_reset(
    datname text DEFAULT NULL,
    usename text DEFAULT NULL
) RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C VOLATILE;

REVOKE ALL ON FUNCTION pg_stat_conn_reset(text, text) FROM PUBLIC;
