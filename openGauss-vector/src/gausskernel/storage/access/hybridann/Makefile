subdir = src/gausskernel/storage/access/hybridann
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif
OBJS = hybridann.o hybridann_build.o hybridann_insert.o hybridann_scan.o hybridann_utils.o hybridann_vacuum.o

override CPPFLAGS += -Wno-volatile -Wno-register -fno-exceptions -I$(OPENBLAS_INCLUDE_PATH) -std=c++17
include $(top_srcdir)/src/gausskernel/common.mk