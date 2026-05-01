subdir = src/gausskernel/storage/access/annvector/distance/x86
top_builddir = ../../../../../../../
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif

SUBDIRS += sse avx avx512

include $(top_srcdir)/src/gausskernel/common.mk
