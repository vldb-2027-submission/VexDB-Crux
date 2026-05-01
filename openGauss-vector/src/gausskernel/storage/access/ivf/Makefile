
subdir = src/gausskernel/storage/access/ivf
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif

OBJS = ivfbuild.o ivf.o ivfinsert.o ivfscan.o ivfutils.o ivfvacuum.o ivfxlog.o ivfinspect.o

override CPPFLAGS += -Wno-invalid-offsetof -Wno-volatile -Wno-register -fno-exceptions -std=c++17
include $(top_srcdir)/src/gausskernel/common.mk
