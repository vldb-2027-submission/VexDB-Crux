#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for embedding
#
# IDENTIFICATION
#    embedding/Makefile
#
#-------------------------------------------------------------------------

subdir = src/gausskernel/storage/access/embedding
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif

OBJS = embedding.o

include $(top_srcdir)/src/gausskernel/common.mk