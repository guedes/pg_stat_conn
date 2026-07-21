/*
 * pg_stat_conn.c
 *		Per (database, user) connection/disconnection statistics.
 *
 * Two storage backends, selected at compile time:
 *
 *  - PG_VERSION_NUM < 180000: a private shared memory hash table
 *    (ShmemInitHash) with our own LWLock tranche, exactly as before PG18
 *    introduced a generic facility for this.
 *
 *  - PG_VERSION_NUM >= 180000: the Cumulative Statistics System's "custom
 *    stats kind" facility (pgstat_register_kind()), which owns the shared
 *    memory (dynamically sized, via DSA), the per-entry locking, and
 *    on-disk persistence across restarts (see
 *    https://wiki.postgresql.org/wiki/CustomCumulativeStats). Modeled after
 *    src/test/modules/injection_points/injection_stats.c in the PostgreSQL
 *    18 tree.
 *
 * Both backends are keyed by the raw database/user names taken from the
 * startup packet (Port->database_name / Port->user_name), not by Oid:
 * ClientAuthentication_hook fires before InitPostgres() resolves
 * MyDatabaseId or the role Oid (see src/backend/utils/init/postinit.c,
 * PerformAuthentication() vs. the MyDatabaseId assignment), and
 * failed-authentication attempts may not even correspond to a real
 * database/role. The pgstat backend hashes the pair into the uint64 objid
 * that PgStat_HashKey requires, and keeps the original strings in the
 * entry's payload (the hash key itself has no room for them).
 *
 * Event capture (pgsc_client_authentication / pgsc_on_disconnect) is shared
 * between both backends; only the storage primitives underneath
 * (pgsc_note_connect / pgsc_note_disconnect / pgsc_entry_reset) differ. In
 * particular the before_shmem_exit()-vs-on_proc_exit() fix described below
 * applies to both.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "funcapi.h"
#include "libpq/auth.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

#if PG_VERSION_NUM >= 180000
#include "common/hashfn.h"
#include "pgstat.h"
#include "utils/pgstat_internal.h"
#endif

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(
					 .name = "pg_stat_conn",
					 .version = PG_VERSION
);
#else
PG_MODULE_MAGIC;
#endif

/* Saved hooks */
static ClientAuthentication_hook_type prev_client_auth_hook = NULL;

void		_PG_init(void);

static void pgsc_client_authentication(Port *port, int status);
static void pgsc_on_disconnect(int code, Datum arg);

/* Implemented once per storage backend, below. */
static void pgsc_note_connect(const char *datname, const char *usename,
							  int status, TimestampTz now);
static void pgsc_note_disconnect(const char *datname, const char *usename,
								 TimestampTz now);
static void pgsc_entry_reset(const char *datname, const char *usename);

PG_FUNCTION_INFO_V1(pg_stat_conn);
PG_FUNCTION_INFO_V1(pg_stat_conn_reset);

#define PGSC_COLS	7

/* ------------------------------------------------------------------------
 * Storage backend: pre-PG18, private shared memory hash table.
 * ------------------------------------------------------------------------
 */
#if PG_VERSION_NUM < 180000

/* GUC */
static int	pgsc_max_entries = 128;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/*
 * Hash key: raw names as supplied at connection time, not resolved Oids.
 */
typedef struct PgStatConnKey
{
	char		datname[NAMEDATALEN];
	char		usename[NAMEDATALEN];
} PgStatConnKey;

typedef struct PgStatConnEntry
{
	PgStatConnKey key;			/* must be first, HASH_BLOBS compares raw bytes */
	pg_atomic_uint64 n_connections;
	pg_atomic_uint64 n_disconnections;
	pg_atomic_uint64 n_auth_failures;
	pg_atomic_uint64 last_connection_time;		/* TimestampTz bits; 0 = never */
	pg_atomic_uint64 last_disconnection_time;	/* TimestampTz bits; 0 = never */
} PgStatConnEntry;

typedef struct PgStatConnShared
{
	LWLock	   *lock;			/* protects the hash table and the fields below */
	TimestampTz stats_reset;
	bool		max_entries_warned;
} PgStatConnShared;

static PgStatConnShared *pgsc = NULL;
static HTAB *pgsc_hash = NULL;

static void pgsc_shmem_request(void);
static void pgsc_shmem_startup(void);
static void pgsc_build_key(PgStatConnKey *key, const char *datname, const char *usename);

static Size
pgsc_memsize(void)
{
	Size		sz;

	sz = MAXALIGN(sizeof(PgStatConnShared));
	sz = add_size(sz, hash_estimate_size(pgsc_max_entries, sizeof(PgStatConnEntry)));
	return sz;
}

static void
pgsc_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(pgsc_memsize());
	RequestNamedLWLockTranche("pg_stat_conn", 1);
}

static void
pgsc_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	pgsc = NULL;
	pgsc_hash = NULL;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgsc = ShmemInitStruct("pg_stat_conn",
							sizeof(PgStatConnShared),
							&found);

	if (!found)
	{
		pgsc->lock = &(GetNamedLWLockTranche("pg_stat_conn"))->lock;
		pgsc->stats_reset = GetCurrentTimestamp();
		pgsc->max_entries_warned = false;
	}

	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(PgStatConnKey);
	info.entrysize = sizeof(PgStatConnEntry);

#if PG_VERSION_NUM >= 190000
	pgsc_hash = ShmemInitHash("pg_stat_conn hash",
							   pgsc_max_entries,
							   &info,
							   HASH_ELEM | HASH_BLOBS);
#else
	pgsc_hash = ShmemInitHash("pg_stat_conn hash",
							   pgsc_max_entries, pgsc_max_entries,
							   &info,
							   HASH_ELEM | HASH_BLOBS);
#endif

	LWLockRelease(AddinShmemInitLock);
}

static void
pgsc_build_key(PgStatConnKey *key, const char *datname, const char *usename)
{
	MemSet(key, 0, sizeof(PgStatConnKey));
	strlcpy(key->datname, datname, NAMEDATALEN);
	strlcpy(key->usename, usename, NAMEDATALEN);
}

static void
pgsc_entry_init(PgStatConnEntry *entry)
{
	pg_atomic_init_u64(&entry->n_connections, 0);
	pg_atomic_init_u64(&entry->n_disconnections, 0);
	pg_atomic_init_u64(&entry->n_auth_failures, 0);
	pg_atomic_init_u64(&entry->last_connection_time, 0);
	pg_atomic_init_u64(&entry->last_disconnection_time, 0);
}

static void
pgsc_apply_connect(PgStatConnEntry *entry, int status, TimestampTz now)
{
	if (status == STATUS_OK)
	{
		pg_atomic_fetch_add_u64(&entry->n_connections, 1);
		pg_atomic_write_u64(&entry->last_connection_time, (uint64) now);
	}
	else
		pg_atomic_fetch_add_u64(&entry->n_auth_failures, 1);
}

static void
pgsc_apply_disconnect(PgStatConnEntry *entry, TimestampTz now)
{
	pg_atomic_fetch_add_u64(&entry->n_disconnections, 1);
	pg_atomic_write_u64(&entry->last_disconnection_time, (uint64) now);
}

/*
 * Find-or-create the entry for key and hand it to apply(), taking the cheapest
 * lock that's safe to do so.
 *
 * Fast path (the common case once every (database, user) pair has been seen at
 * least once): look the entry up under LW_SHARED, which many backends can hold
 * concurrently, and mutate it with atomic ops while still holding that shared
 * lock releasing it only after the update would let a concurrent
 * pg_stat_conn_reset() (which needs LW_EXCLUSIVE to HASH_REMOVE) free the entry
 * out from under us mid-update, holding LW_SHARED is what prevents that race,
 * since LW_EXCLUSIVE can't be granted while any shared holder is in.
 *
 * Slow path: only reached the first time a given pair is seen, or if a reset
 * raced the entry away between our shared lookup and getting here.  Needs
 * LW_EXCLUSIVE to insert, exactly once per distinct pair.
 */
static void
pgsc_record(const PgStatConnKey *key,
			void (*apply) (PgStatConnEntry *entry, void *arg), void *arg)
{
	PgStatConnEntry *entry;
	bool		found;

	LWLockAcquire(pgsc->lock, LW_SHARED);
	entry = (PgStatConnEntry *) hash_search(pgsc_hash, key, HASH_FIND, NULL);
	if (entry != NULL)
	{
		apply(entry, arg);
		LWLockRelease(pgsc->lock);
		return;
	}
	LWLockRelease(pgsc->lock);

	LWLockAcquire(pgsc->lock, LW_EXCLUSIVE);
	entry = (PgStatConnEntry *) hash_search(pgsc_hash, key, HASH_ENTER_NULL, &found);
	if (entry == NULL)
	{
		bool		must_warn = !pgsc->max_entries_warned;

		if (must_warn)
			pgsc->max_entries_warned = true;
		LWLockRelease(pgsc->lock);
		if (must_warn)
			ereport(WARNING,
					(errmsg("pg_stat_conn: maximum number of tracked (database, user) pairs (%d) reached",
							pgsc_max_entries),
					 errhint("Increase pg_stat_conn.max_entries or reset pg_stat_conn to free space.")));
		return;
	}

	/* Not an error for found to already be true: another backend may have
	 * inserted it in the gap between our shared lookup and this exclusive
	 * acquire. */
	if (!found)
		pgsc_entry_init(entry);
	apply(entry, arg);
	LWLockRelease(pgsc->lock);
}

struct pgsc_connect_arg
{
	int			status;
	TimestampTz now;
};

static void
pgsc_record_connect_cb(PgStatConnEntry *entry, void *arg)
{
	struct pgsc_connect_arg *a = (struct pgsc_connect_arg *) arg;

	pgsc_apply_connect(entry, a->status, a->now);
}

static void
pgsc_record_disconnect_cb(PgStatConnEntry *entry, void *arg)
{
	pgsc_apply_disconnect(entry, *(TimestampTz *) arg);
}

static void
pgsc_note_connect(const char *datname, const char *usename, int status, TimestampTz now)
{
	PgStatConnKey key;
	struct pgsc_connect_arg arg;

	if (!pgsc || !pgsc_hash)
		return;

	pgsc_build_key(&key, datname, usename);
	arg.status = status;
	arg.now = now;

	pgsc_record(&key, pgsc_record_connect_cb, &arg);
}

static void
pgsc_note_disconnect(const char *datname, const char *usename, TimestampTz now)
{
	PgStatConnKey key;

	if (!pgsc || !pgsc_hash)
		return;

	pgsc_build_key(&key, datname, usename);
	pgsc_record(&key, pgsc_record_disconnect_cb, (void *) &now);
}

static void
pgsc_entry_reset(const char *datname, const char *usename)
{
	HASH_SEQ_STATUS hash_seq;
	PgStatConnEntry *entry;

	if (!pgsc || !pgsc_hash)
		return;

	LWLockAcquire(pgsc->lock, LW_EXCLUSIVE);

	if (datname != NULL && usename != NULL)
	{
		PgStatConnKey key;

		MemSet(&key, 0, sizeof(key));
		strlcpy(key.datname, datname, NAMEDATALEN);
		strlcpy(key.usename, usename, NAMEDATALEN);
		hash_search(pgsc_hash, &key, HASH_REMOVE, NULL);
	}
	else if (datname != NULL || usename != NULL)
	{
		hash_seq_init(&hash_seq, pgsc_hash);
		while ((entry = (PgStatConnEntry *) hash_seq_search(&hash_seq)) != NULL)
		{
			if ((datname == NULL || strcmp(entry->key.datname, datname) == 0) &&
				(usename == NULL || strcmp(entry->key.usename, usename) == 0))
				hash_search(pgsc_hash, &entry->key, HASH_REMOVE, NULL);
		}
	}
	else
	{
		hash_seq_init(&hash_seq, pgsc_hash);
		while ((entry = (PgStatConnEntry *) hash_seq_search(&hash_seq)) != NULL)
			hash_search(pgsc_hash, &entry->key, HASH_REMOVE, NULL);

		pgsc->stats_reset = GetCurrentTimestamp();
	}

	/* Space may have been freed up. */
	pgsc->max_entries_warned = false;

	LWLockRelease(pgsc->lock);
}

Datum
pg_stat_conn(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS hash_seq;
	PgStatConnEntry *entry;

	if (!pgsc || !pgsc_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_conn must be loaded via \"shared_preload_libraries\"")));

#if PG_VERSION_NUM >= 160000
	InitMaterializedSRF(fcinfo, 0);
#else
	SetSingleFuncCall(fcinfo, 0);
#endif

	LWLockAcquire(pgsc->lock, LW_SHARED);

	hash_seq_init(&hash_seq, pgsc_hash);
	while ((entry = (PgStatConnEntry *) hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[PGSC_COLS];
		bool		nulls[PGSC_COLS];
		int			i = 0;
		TimestampTz last_connection_time,
					last_disconnection_time;

		MemSet(nulls, 0, sizeof(nulls));

		values[i++] = CStringGetTextDatum(entry->key.datname);
		values[i++] = CStringGetTextDatum(entry->key.usename);
		values[i++] = Int64GetDatum((int64) pg_atomic_read_u64(&entry->n_connections));
		values[i++] = Int64GetDatum((int64) pg_atomic_read_u64(&entry->n_disconnections));
		values[i++] = Int64GetDatum((int64) pg_atomic_read_u64(&entry->n_auth_failures));

		last_connection_time = (TimestampTz) pg_atomic_read_u64(&entry->last_connection_time);
		last_disconnection_time = (TimestampTz) pg_atomic_read_u64(&entry->last_disconnection_time);

		if (last_connection_time == 0)
			nulls[i++] = true;
		else
			values[i++] = TimestampTzGetDatum(last_connection_time);

		if (last_disconnection_time == 0)
			nulls[i++] = true;
		else
			values[i++] = TimestampTzGetDatum(last_disconnection_time);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	LWLockRelease(pgsc->lock);

	return (Datum) 0;
}

Datum
pg_stat_conn_reset(PG_FUNCTION_ARGS)
{
	char	   *datname = PG_ARGISNULL(0) ? NULL : TextDatumGetCString(PG_GETARG_DATUM(0));
	char	   *usename = PG_ARGISNULL(1) ? NULL : TextDatumGetCString(PG_GETARG_DATUM(1));

	if (!pgsc || !pgsc_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_conn must be loaded via \"shared_preload_libraries\"")));

	pgsc_entry_reset(datname, usename);

	PG_RETURN_VOID();
}

#else							/* PG_VERSION_NUM >= 180000 */

/* ------------------------------------------------------------------------
 * Storage backend: PG18+, Cumulative Statistics System custom stats kind.
 *
 * Custom stats kind IDs are a scarce, global, compile-time resource shared
 * by every extension that uses this facility on a given build
 * (PGSTAT_KIND_CUSTOM_MIN..PGSTAT_KIND_CUSTOM_MAX, 9 slots as of PG18) --
 * see https://wiki.postgresql.org/wiki/CustomCumulativeStats. We default to
 * PGSTAT_KIND_EXPERIMENTAL; packagers who have reserved a permanent ID on
 * that wiki page should build with -DPGSC_STATS_KIND=<reserved id>.
 * ------------------------------------------------------------------------
 */

#ifndef PGSC_STATS_KIND
#define PGSC_STATS_KIND PGSTAT_KIND_EXPERIMENTAL
#endif

/* Track if our stats kind has been registered. */
static bool pgsc_connstat_loaded = false;

/* Shared, persisted per-(database, user) stats entry. */
typedef struct PgStat_StatConnEntry
{
	char		datname[NAMEDATALEN];
	char		usename[NAMEDATALEN];
	PgStat_Counter n_connections;
	PgStat_Counter n_disconnections;
	PgStat_Counter n_auth_failures;
	TimestampTz last_connection_time;		/* 0 = never */
	TimestampTz last_disconnection_time;	/* 0 = never */
} PgStat_StatConnEntry;

typedef struct PgStatShared_ConnStat
{
	PgStatShared_Common header;
	PgStat_StatConnEntry stats;
} PgStatShared_ConnStat;

/* Backend-local delta, merged into the shared entry by pgsc_flush_cb(). */
typedef struct PgStat_StatConnPending
{
	PgStat_Counter n_connections;
	PgStat_Counter n_disconnections;
	PgStat_Counter n_auth_failures;
	TimestampTz last_connection_time;
	TimestampTz last_disconnection_time;
} PgStat_StatConnPending;

static bool pgsc_flush_cb(PgStat_EntryRef *entry_ref, bool nowait);

static const PgStat_KindInfo pgsc_stats = {
	.name = "pg_stat_conn",
	.fixed_amount = false,		/* one entry per (database, user) pair seen */
	.accessed_across_databases = true,		/* not scoped to a single database */
	.write_to_file = true,		/* survive clean restarts */

	.shared_size = sizeof(PgStatShared_ConnStat),
	.shared_data_off = offsetof(PgStatShared_ConnStat, stats),
	.shared_data_len = sizeof(((PgStatShared_ConnStat *) 0)->stats),
	.pending_size = sizeof(PgStat_StatConnPending),
	.flush_pending_cb = pgsc_flush_cb,
};

/*
 * PgStat_HashKey has no room for the raw datname/usename strings (only a
 * uint64 objid), so hash the pair into one. Chained rather than
 * concatenated, so that ("ab","c") and ("a","bc") don't collide.
 */
static uint64
pgsc_objid(const char *datname, const char *usename)
{
	uint64		h;

	h = hash_bytes_extended((const unsigned char *) datname, strlen(datname), 0);
	h = hash_bytes_extended((const unsigned char *) usename, strlen(usename), h);
	return h;
}

static bool
pgsc_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStat_StatConnPending *pending = (PgStat_StatConnPending *) entry_ref->pending;
	PgStatShared_ConnStat *shconn = (PgStatShared_ConnStat *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	shconn->stats.n_connections += pending->n_connections;
	shconn->stats.n_disconnections += pending->n_disconnections;
	shconn->stats.n_auth_failures += pending->n_auth_failures;
	if (pending->last_connection_time != 0)
		shconn->stats.last_connection_time = pending->last_connection_time;
	if (pending->last_disconnection_time != 0)
		shconn->stats.last_disconnection_time = pending->last_disconnection_time;

	pgstat_unlock_entry(entry_ref);

	return true;
}

/*
 * (Re-)stamp the entry's identity fields whenever pgstat_prep_pending_entry()
 * tells us it (re-)created the shared entry.
 *
 * created_entry is true both the first time a (database, user) pair is seen
 * and whenever the entry is recreated after having been dropped (e.g. by
 * pg_stat_conn_reset()). Either the connect or the disconnect event can be
 * the one that ends up (re)creating the entry -- e.g. a session that resets
 * its own row and then disconnects recreates it via the disconnect path --
 * so both pgsc_note_connect() and pgsc_note_disconnect() must call this,
 * not just connect.
 */
static void
pgsc_maybe_init_identity(PgStat_EntryRef *entry_ref, bool created_entry,
						 const char *datname, const char *usename)
{
	PgStatShared_ConnStat *shconn;

	if (!created_entry)
		return;

	shconn = (PgStatShared_ConnStat *) entry_ref->shared_stats;

	pgstat_lock_entry(entry_ref, false);
	strlcpy(shconn->stats.datname, datname, NAMEDATALEN);
	strlcpy(shconn->stats.usename, usename, NAMEDATALEN);
	pgstat_unlock_entry(entry_ref);
}

static void
pgsc_note_connect(const char *datname, const char *usename, int status, TimestampTz now)
{
	PgStat_EntryRef *entry_ref;
	bool		created_entry;
	PgStat_StatConnPending *pending;

	if (!pgsc_connstat_loaded)
		return;

	entry_ref = pgstat_prep_pending_entry(PGSC_STATS_KIND, InvalidOid,
										  pgsc_objid(datname, usename),
										  &created_entry);

	pgsc_maybe_init_identity(entry_ref, created_entry, datname, usename);

	pending = (PgStat_StatConnPending *) entry_ref->pending;

	if (status == STATUS_OK)
	{
		pending->n_connections++;
		pending->last_connection_time = now;
	}
	else
		pending->n_auth_failures++;
}

static void
pgsc_note_disconnect(const char *datname, const char *usename, TimestampTz now)
{
	PgStat_EntryRef *entry_ref;
	bool		created_entry;
	PgStat_StatConnPending *pending;

	if (!pgsc_connstat_loaded)
		return;

	entry_ref = pgstat_prep_pending_entry(PGSC_STATS_KIND, InvalidOid,
										  pgsc_objid(datname, usename),
										  &created_entry);

	pgsc_maybe_init_identity(entry_ref, created_entry, datname, usename);

	pending = (PgStat_StatConnPending *) entry_ref->pending;
	pending->n_disconnections++;
	pending->last_disconnection_time = now;
}

/* Filter used by pgsc_match_reset() below; NULL fields mean "any". */
struct pgsc_reset_filter
{
	const char *datname;
	const char *usename;
};

static bool
pgsc_match_reset(PgStatShared_HashEntry *entry, Datum match_data)
{
	struct pgsc_reset_filter *filter = (struct pgsc_reset_filter *) DatumGetPointer(match_data);
	PgStatShared_ConnStat *shconn;

	if (entry->key.kind != PGSC_STATS_KIND)
		return false;

	shconn = (PgStatShared_ConnStat *) dsa_get_address(pgStatLocal.dsa, entry->body);

	if (filter->datname != NULL && strcmp(shconn->stats.datname, filter->datname) != 0)
		return false;
	if (filter->usename != NULL && strcmp(shconn->stats.usename, filter->usename) != 0)
		return false;

	return true;
}

/*
 * Reset semantics match the pre-PG18 backend: entries are dropped outright
 * (not zeroed in place), so a reset (database, user) pair reappears fresh on
 * its next connection. This also sidesteps pgstat_reset_entry()'s built-in
 * "zero the whole shared_data region" behavior, which would erase the
 * datname/usename we keep alongside the counters.
 */
static void
pgsc_entry_reset(const char *datname, const char *usename)
{
	if (!pgsc_connstat_loaded)
		return;

	if (datname != NULL && usename != NULL)
		pgstat_drop_entry(PGSC_STATS_KIND, InvalidOid, pgsc_objid(datname, usename));
	else
	{
		struct pgsc_reset_filter filter;

		filter.datname = datname;
		filter.usename = usename;
		pgstat_drop_matching_entries(pgsc_match_reset, PointerGetDatum(&filter));
	}
}

Datum
pg_stat_conn(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	dshash_seq_status hstat;
	PgStatShared_HashEntry *entry;

	if (!pgsc_connstat_loaded)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_conn must be loaded via \"shared_preload_libraries\"")));

	InitMaterializedSRF(fcinfo, 0);

	dshash_seq_init(&hstat, pgStatLocal.shared_hash, false);
	while ((entry = dshash_seq_next(&hstat)) != NULL)
	{
		PgStatShared_ConnStat *shconn;
		Datum		values[PGSC_COLS];
		bool		nulls[PGSC_COLS];
		int			i = 0;
		TimestampTz last_connection_time,
					last_disconnection_time;

		if (entry->dropped || entry->key.kind != PGSC_STATS_KIND)
			continue;

		shconn = (PgStatShared_ConnStat *) dsa_get_address(pgStatLocal.dsa, entry->body);

		LWLockAcquire(&shconn->header.lock, LW_SHARED);

		MemSet(nulls, 0, sizeof(nulls));

		values[i++] = CStringGetTextDatum(shconn->stats.datname);
		values[i++] = CStringGetTextDatum(shconn->stats.usename);
		values[i++] = Int64GetDatum(shconn->stats.n_connections);
		values[i++] = Int64GetDatum(shconn->stats.n_disconnections);
		values[i++] = Int64GetDatum(shconn->stats.n_auth_failures);

		last_connection_time = shconn->stats.last_connection_time;
		last_disconnection_time = shconn->stats.last_disconnection_time;

		if (last_connection_time == 0)
			nulls[i++] = true;
		else
			values[i++] = TimestampTzGetDatum(last_connection_time);

		if (last_disconnection_time == 0)
			nulls[i++] = true;
		else
			values[i++] = TimestampTzGetDatum(last_disconnection_time);

		LWLockRelease(&shconn->header.lock);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	dshash_seq_term(&hstat);

	return (Datum) 0;
}

Datum
pg_stat_conn_reset(PG_FUNCTION_ARGS)
{
	char	   *datname = PG_ARGISNULL(0) ? NULL : TextDatumGetCString(PG_GETARG_DATUM(0));
	char	   *usename = PG_ARGISNULL(1) ? NULL : TextDatumGetCString(PG_GETARG_DATUM(1));

	if (!pgsc_connstat_loaded)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_conn must be loaded via \"shared_preload_libraries\"")));

	pgsc_entry_reset(datname, usename);

	PG_RETURN_VOID();
}

#endif							/* PG_VERSION_NUM >= 180000 */

/* ------------------------------------------------------------------------
 * Event capture: shared by both storage backends.
 * ------------------------------------------------------------------------
 */

static void
pgsc_client_authentication(Port *port, int status)
{
	const char *datname = port->database_name ? port->database_name : "";
	const char *usename = port->user_name ? port->user_name : "";

	if (prev_client_auth_hook)
		prev_client_auth_hook(port, status);

	pgsc_note_connect(datname, usename, status, GetCurrentTimestamp());

	/*
	 * Only track disconnection for sessions that actually authenticated.
	 *
	 * Use before_shmem_exit(), not on_proc_exit(): ProcKill() (which sets
	 * MyProc = NULL) is itself registered via on_shmem_exit(), and
	 * shmem_exit() runs the before_shmem_exit list before the
	 * on_shmem_exit list, while proc_exit_prepare() only runs the
	 * on_proc_exit list after shmem_exit() has already completed. By the
	 * time an on_proc_exit callback runs, MyProc is NULL, and
	 * LWLockAcquire() cannot block on it, so it panics with "cannot wait
	 * without a PGPROC structure" whenever the lock happens to be
	 * contended at disconnect time.
	 *
	 * before_shmem_exit() callbacks run LIFO. On PG18+, this hook (which
	 * updates the pending stats entry) is registered here, after
	 * BaseInit()'s pgstat_initialize() already registered pgstat's own
	 * before_shmem_exit(pgstat_shutdown_hook), so ours runs first and
	 * pgstat's final pgstat_report_stat() flush -- which applies
	 * pgsc_flush_cb() -- runs after, picking up our update even for a
	 * connection that never runs a query.
	 */
	if (status == STATUS_OK)
		before_shmem_exit(pgsc_on_disconnect, (Datum) 0);
}

static void
pgsc_on_disconnect(int code, Datum arg)
{
	Port	   *port = MyProcPort;

	if (port == NULL)
		return;

	pgsc_note_disconnect(port->database_name ? port->database_name : "",
						  port->user_name ? port->user_name : "",
						  GetCurrentTimestamp());
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

#if PG_VERSION_NUM < 180000
	DefineCustomIntVariable("pg_stat_conn.max_entries",
							 "Sets the maximum number of tracked (database, user) pairs.",
							 NULL,
							 &pgsc_max_entries,
							 128,
							 1,
							 1000000,
							 PGC_POSTMASTER,
							 0,
							 NULL, NULL, NULL);
#endif

	MarkGUCPrefixReserved("pg_stat_conn");

#if PG_VERSION_NUM < 180000
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pgsc_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgsc_shmem_startup;
#else
	pgstat_register_kind(PGSC_STATS_KIND, &pgsc_stats);
	pgsc_connstat_loaded = true;
#endif

	prev_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = pgsc_client_authentication;
}
