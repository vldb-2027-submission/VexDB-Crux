#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for annvector
#
# IDENTIFICATION
#    annvector/Makefile
#
#-------------------------------------------------------------------------

subdir = src/gausskernel/storage/access/annvector
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif
OPTIMIZE_OPTIONS = -ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math
override CPPFLAGS += $(OPTIMIZE_OPTIONS)

OBJS = floatvector.o halfvec.o annkmeans.o pq.o ann_utils.o quantizer.o sparsevector.o
SUBDIRS += distance xlog module

include $(top_srcdir)/src/gausskernel/common.mk