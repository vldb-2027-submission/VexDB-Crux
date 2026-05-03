MODULE_big = crux_repair
EXTENSION = crux_repair
DATA = crux_repair--1.0.sql

OBJS = crux_repair.o

CRUX_INC_DIR = $(top_srcdir)/src/gausskernel/storage/access/crux

override CPPFLAGS := -I$(CRUX_INC_DIR) -I. $(CPPFLAGS) -Wno-volatile -Wno-register -fno-exceptions -std=c++17

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/crux_repair
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif