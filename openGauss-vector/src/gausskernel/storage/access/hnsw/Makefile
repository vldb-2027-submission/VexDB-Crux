
subdir = src/gausskernel/storage/access/hnsw
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif
OBJS = hnsw.o hnswbuild.o hnswinsert.o hnswquantizer.o hnswscan.o hnswutils.o hnswvacuum.o hnswxlog.o hnswinspect.o

override CPPFLAGS += -std=c++17

include $(top_srcdir)/src/gausskernel/common.mk

