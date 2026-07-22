MODULES = pg_stat_conn
EXTENSION = pg_stat_conn
DATA = pg_stat_conn--1.0.sql
PGFILEDESC = "pg_stat_conn - per (database, user) connection statistics"

# All behavior worth testing requires shared_preload_libraries and
# distinct connect/disconnect events, which pg_regress's single
# long-lived test session cannot exercise; TAP tests spin up their own
# purpose-configured cluster instead. See t/.
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_stat_conn
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
