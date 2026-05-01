#-------------------------------------------------------------------------
#
# Makefile for src/bin/vex_controldata
#
# Copyright (c) 1998-2012, PostgreSQL Global Development Group
#
# src/bin/vex_controldata/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "vex_controldata - reads the data from pg_control"
PGAPPICON=win32

subdir = src/bin/vex_controldata
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
    ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
      -include $(DEPEND)
    endif
  endif
endif
OBJS= pg_controldata.o $(WIN32RES) $(top_builddir)/src/lib/elog/elog.a \
	$(top_builddir)/src/gausskernel/storage/dss/dss_adaptor.o

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS) -fPIC -fPIE 

all: vex_controldata

vex_controldata: $(OBJS) | submake-libpgport
	$(CC) $(CXXFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

$(top_builddir)/src/lib/elog/elog.a:
	$(MAKE) -C $(top_builddir)/src/lib/elog elog.a

install: all installdirs
	$(INSTALL_PROGRAM) vex_controldata$(X) '$(DESTDIR)$(bindir)/vex_controldata$(X)'
	rm -f '$(DESTDIR)$(bindir)/pg_controldata$(X)'
	ln -s vex_controldata$(X) '$(DESTDIR)$(bindir)/pg_controldata$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/vex_controldata$(X)'
	rm -f '$(DESTDIR)$(bindir)/pg_controldata$(X)'

clean distclean maintainer-clean:
	rm -f vex_controldata$(X) pg_controldata$(X) $(OBJS) *.depend
