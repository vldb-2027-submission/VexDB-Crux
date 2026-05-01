#-------------------------------------------------------------------------
#
# Makefile for src/bin/vex_resetxlog
#
# Copyright (c) 1998-2012, PostgreSQL Global Development Group
#
# src/bin/vex_resetxlog/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "vex_resetxlog - reset PostgreSQL WAL log"
PGAPPICON=win32

subdir = src/bin/vex_resetxlog
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
    ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
      -include $(DEPEND)
    endif
  endif
endif
OBJS= pg_resetxlog.o $(WIN32RES)
OBJS += $(top_builddir)/src/gausskernel/storage/dss/dss_adaptor.o

all: vex_resetxlog

override CXXFLAGS += -fPIE
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS) -I${top_builddir}/src/include
CFLAGS += -Wl,-z,relro,-z,now -fPIE
LDFLAGS += -pie

vex_resetxlog: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) vex_resetxlog$(X) '$(DESTDIR)$(bindir)/vex_resetxlog$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/vex_resetxlog$(X)'

clean distclean maintainer-clean:
	rm -f vex_resetxlog$(X) $(OBJS) *.depend
