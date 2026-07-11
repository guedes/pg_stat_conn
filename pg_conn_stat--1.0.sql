-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_conn_stat" to load this file. \quit

CREATE FUNCTION pg_conn_stat(
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

REVOKE ALL ON FUNCTION pg_conn_stat() FROM PUBLIC;

CREATE VIEW pg_conn_stat AS
    SELECT * FROM pg_conn_stat();

GRANT SELECT ON pg_conn_stat TO pg_read_all_stats;

CREATE FUNCTION pg_conn_stat_reset(
    datname text DEFAULT NULL,
    usename text DEFAULT NULL
) RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C VOLATILE;

REVOKE ALL ON FUNCTION pg_conn_stat_reset(text, text) FROM PUBLIC;
