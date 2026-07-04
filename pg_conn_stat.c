/*
 * pg_conn_stat.c
 *		Per (database, user) connection/disconnection statistics.
 *
 * Tracked in a fixed-size shared memory hash table keyed by the raw
 * database/user names taken from the startup packet (Port->database_name /
 * Port->user_name), not by Oid: ClientAuthentication_hook fires before
 * InitPostgres() resolves MyDatabaseId or the role Oid (see
 * src/backend/utils/init/postinit.c, PerformAuthentication() at line 948
 * vs. MyDatabaseId assignment at line 1167), and failed-authentication
 * attempts may not even correspond to a real database/role.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "funcapi.h"
#include "libpq/auth.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(
					 .name = "pg_conn_stat",
					 .version = PG_VERSION
);
#else
PG_MODULE_MAGIC;
#endif

/* GUC */
static int	pgcs_max_entries = 128;

/* Saved hooks */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ClientAuthentication_hook_type prev_client_auth_hook = NULL;

/*
 * Hash key: raw names as supplied at connection time, not resolved Oids.
 */
typedef struct PgConnStatKey
{
	char		datname[NAMEDATALEN];
	char		usename[NAMEDATALEN];
} PgConnStatKey;

typedef struct PgConnStatEntry
{
	PgConnStatKey key;			/* must be first, HASH_BLOBS compares raw bytes */
	int64		n_connections;
	int64		n_disconnections;
	int64		n_auth_failures;
	TimestampTz last_connection_time;		/* 0 = never */
	TimestampTz last_disconnection_time;	/* 0 = never */
} PgConnStatEntry;

typedef struct PgConnStatShared
{
	LWLock	   *lock;			/* protects the hash table and the fields below */
	TimestampTz stats_reset;
	bool		max_entries_warned;
} PgConnStatShared;

static PgConnStatShared *pgcs = NULL;
static HTAB *pgcs_hash = NULL;

void		_PG_init(void);

static void pgcs_shmem_request(void);
static void pgcs_shmem_startup(void);
static void pgcs_client_authentication(Port *port, int status);
static void pgcs_on_disconnect(int code, Datum arg);
static void pgcs_build_key(PgConnStatKey *key, const Port *port);
static void pgcs_entry_reset(const char *datname, const char *usename);

PG_FUNCTION_INFO_V1(pg_conn_stat);
PG_FUNCTION_INFO_V1(pg_conn_stat_reset);

static Size
pgcs_memsize(void)
{
	Size		sz;

	sz = MAXALIGN(sizeof(PgConnStatShared));
	sz = add_size(sz, hash_estimate_size(pgcs_max_entries, sizeof(PgConnStatEntry)));
	return sz;
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomIntVariable("pg_conn_stat.max_entries",
							 "Sets the maximum number of tracked (database, user) pairs.",
							 NULL,
							 &pgcs_max_entries,
							 128,
							 1,
							 1000000,
							 PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("pg_conn_stat");

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pgcs_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgcs_shmem_startup;

	prev_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = pgcs_client_authentication;
}

static void
pgcs_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(pgcs_memsize());
	RequestNamedLWLockTranche("pg_conn_stat", 1);
}

static void
pgcs_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	pgcs = NULL;
	pgcs_hash = NULL;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgcs = ShmemInitStruct("pg_conn_stat",
							sizeof(PgConnStatShared),
							&found);

	if (!found)
	{
		pgcs->lock = &(GetNamedLWLockTranche("pg_conn_stat"))->lock;
		pgcs->stats_reset = GetCurrentTimestamp();
		pgcs->max_entries_warned = false;
	}

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(PgConnStatKey);
	info.entrysize = sizeof(PgConnStatEntry);

#if PG_VERSION_NUM >= 190000
	pgcs_hash = ShmemInitHash("pg_conn_stat hash",
							   pgcs_max_entries,
							   &info,
							   HASH_ELEM | HASH_BLOBS);
#else
	pgcs_hash = ShmemInitHash("pg_conn_stat hash",
							   pgcs_max_entries, pgcs_max_entries,
							   &info,
							   HASH_ELEM | HASH_BLOBS);
#endif

	LWLockRelease(AddinShmemInitLock);
}

static void
pgcs_build_key(PgConnStatKey *key, const Port *port)
{
	MemSet(key, 0, sizeof(PgConnStatKey));
	strlcpy(key->datname,
			port->database_name ? port->database_name : "",
			NAMEDATALEN);
	strlcpy(key->usename,
			port->user_name ? port->user_name : "",
			NAMEDATALEN);
}

static void
pgcs_client_authentication(Port *port, int status)
{
	PgConnStatKey key;
	PgConnStatEntry *entry;
	bool		found;
	TimestampTz now;

	if (prev_client_auth_hook)
		prev_client_auth_hook(port, status);

	if (!pgcs || !pgcs_hash)
		return;

	pgcs_build_key(&key, port);
	now = GetCurrentTimestamp();

	LWLockAcquire(pgcs->lock, LW_EXCLUSIVE);

	entry = (PgConnStatEntry *) hash_search(pgcs_hash, &key, HASH_ENTER_NULL, &found);
	if (entry == NULL)
	{
		bool		must_warn = !pgcs->max_entries_warned;

		if (must_warn)
			pgcs->max_entries_warned = true;
		LWLockRelease(pgcs->lock);
		if (must_warn)
			ereport(WARNING,
					(errmsg("pg_conn_stat: maximum number of tracked (database, user) pairs (%d) reached",
							pgcs_max_entries),
					 errhint("Increase pg_conn_stat.max_entries or reset pg_conn_stat to free space.")));
		return;
	}

	if (!found)
	{
		entry->n_connections = 0;
		entry->n_disconnections = 0;
		entry->n_auth_failures = 0;
		entry->last_connection_time = 0;
		entry->last_disconnection_time = 0;
	}

	if (status == STATUS_OK)
	{
		entry->n_connections++;
		entry->last_connection_time = now;
	}
	else
	{
		entry->n_auth_failures++;
	}

	LWLockRelease(pgcs->lock);

	/* Only track disconnection for sessions that actually authenticated. */
	if (status == STATUS_OK)
		on_proc_exit(pgcs_on_disconnect, (Datum) 0);
}

static void
pgcs_on_disconnect(int code, Datum arg)
{
	Port	   *port = MyProcPort;
	PgConnStatKey key;
	PgConnStatEntry *entry;
	bool		found;

	if (!pgcs || !pgcs_hash || port == NULL)
		return;

	pgcs_build_key(&key, port);

	LWLockAcquire(pgcs->lock, LW_EXCLUSIVE);

	/*
	 * HASH_ENTER_NULL rather than HASH_FIND: if a concurrent reset removed
	 * the entry between connect and disconnect, recreate it instead of
	 * silently losing this disconnection.
	 */
	entry = (PgConnStatEntry *) hash_search(pgcs_hash, &key, HASH_ENTER_NULL, &found);
	if (entry == NULL)
	{
		bool		must_warn = !pgcs->max_entries_warned;

		if (must_warn)
			pgcs->max_entries_warned = true;
		LWLockRelease(pgcs->lock);
		if (must_warn)
			ereport(WARNING,
					(errmsg("pg_conn_stat: cannot record disconnection, tracking table is full")));
		return;
	}

	if (!found)
	{
		entry->n_connections = 0;
		entry->n_disconnections = 0;
		entry->n_auth_failures = 0;
		entry->last_connection_time = 0;
		entry->last_disconnection_time = 0;
	}

	entry->n_disconnections++;
	entry->last_disconnection_time = GetCurrentTimestamp();

	LWLockRelease(pgcs->lock);
}

#define PGCS_COLS	7

Datum
pg_conn_stat(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS hash_seq;
	PgConnStatEntry *entry;

	if (!pgcs || !pgcs_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_conn_stat must be loaded via \"shared_preload_libraries\"")));

#if PG_VERSION_NUM >= 160000
	InitMaterializedSRF(fcinfo, 0);
#else
	SetSingleFuncCall(fcinfo, 0);
#endif

	LWLockAcquire(pgcs->lock, LW_SHARED);

	hash_seq_init(&hash_seq, pgcs_hash);
	while ((entry = (PgConnStatEntry *) hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[PGCS_COLS];
		bool		nulls[PGCS_COLS];
		int			i = 0;

		memset(nulls, 0, sizeof(nulls));

		values[i++] = CStringGetTextDatum(entry->key.datname);
		values[i++] = CStringGetTextDatum(entry->key.usename);
		values[i++] = Int64GetDatum(entry->n_connections);
		values[i++] = Int64GetDatum(entry->n_disconnections);
		values[i++] = Int64GetDatum(entry->n_auth_failures);

		if (entry->last_connection_time == 0)
			nulls[i++] = true;
		else
			values[i++] = TimestampTzGetDatum(entry->last_connection_time);

		if (entry->last_disconnection_time == 0)
			nulls[i++] = true;
		else
			values[i++] = TimestampTzGetDatum(entry->last_disconnection_time);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	LWLockRelease(pgcs->lock);

	return (Datum) 0;
}

static void
pgcs_entry_reset(const char *datname, const char *usename)
{
	HASH_SEQ_STATUS hash_seq;
	PgConnStatEntry *entry;

	LWLockAcquire(pgcs->lock, LW_EXCLUSIVE);

	if (datname != NULL && usename != NULL)
	{
		PgConnStatKey key;

		MemSet(&key, 0, sizeof(key));
		strlcpy(key.datname, datname, NAMEDATALEN);
		strlcpy(key.usename, usename, NAMEDATALEN);
		hash_search(pgcs_hash, &key, HASH_REMOVE, NULL);
	}
	else if (datname != NULL || usename != NULL)
	{
		hash_seq_init(&hash_seq, pgcs_hash);
		while ((entry = (PgConnStatEntry *) hash_seq_search(&hash_seq)) != NULL)
		{
			if ((datname == NULL || strcmp(entry->key.datname, datname) == 0) &&
				(usename == NULL || strcmp(entry->key.usename, usename) == 0))
				hash_search(pgcs_hash, &entry->key, HASH_REMOVE, NULL);
		}
	}
	else
	{
		hash_seq_init(&hash_seq, pgcs_hash);
		while ((entry = (PgConnStatEntry *) hash_seq_search(&hash_seq)) != NULL)
			hash_search(pgcs_hash, &entry->key, HASH_REMOVE, NULL);

		pgcs->stats_reset = GetCurrentTimestamp();
	}

	/* Space may have been freed up. */
	pgcs->max_entries_warned = false;

	LWLockRelease(pgcs->lock);
}

Datum
pg_conn_stat_reset(PG_FUNCTION_ARGS)
{
	char	   *datname = PG_ARGISNULL(0) ? NULL : TextDatumGetCString(PG_GETARG_DATUM(0));
	char	   *usename = PG_ARGISNULL(1) ? NULL : TextDatumGetCString(PG_GETARG_DATUM(1));

	if (!pgcs || !pgcs_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_conn_stat must be loaded via \"shared_preload_libraries\"")));

	pgcs_entry_reset(datname, usename);

	PG_RETURN_VOID();
}
