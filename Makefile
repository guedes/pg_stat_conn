MODULES = pg_stat_conn
EXTENSION = pg_stat_conn
DATA = pg_stat_conn--1.0.sql
PGFILEDESC = "pg_stat_conn - per (database, user) connection statistics"

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
