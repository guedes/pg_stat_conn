MODULES = pg_conn_stat
EXTENSION = pg_conn_stat
DATA = pg_conn_stat--1.0.sql
PGFILEDESC = "pg_conn_stat - per (database, user) connection statistics"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_conn_stat
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
