subdir = src/gausskernel/storage/access/diskann
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif
OBJS = diskann.o diskann_internal.o diskann_build.o diskann_change.o diskann_scan.o diskann_utils.o \
	    partition.o disk_pq.o diskannxlog.o math_utils.o diskann_inspect.o

SUBDIRS = storage_interface

override CPPFLAGS += -Wno-volatile -Wno-register -fno-exceptions -I$(OPENBLAS_INCLUDE_PATH) -std=c++17
include $(top_srcdir)/src/gausskernel/common.mk
