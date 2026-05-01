MODULE_big = qasp_repair
EXTENSION = qasp_repair
DATA = qasp_repair--1.0.sql

OBJS = qasp_repair.o

QASP_INC_DIR = $(top_srcdir)/src/gausskernel/storage/access/qasp

override CPPFLAGS := -I$(QASP_INC_DIR) -I. $(CPPFLAGS) -Wno-volatile -Wno-register -fno-exceptions -std=c++17

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/qasp_repair
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif