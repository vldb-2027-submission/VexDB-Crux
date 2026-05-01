subdir = src/gausskernel/storage/access/annvector/distance/general
top_builddir = ../../../../../../../
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif

OPTIMIZE_OPTIONS = -ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -floop-interchange
override CPPFLAGS += $(OPTIMIZE_OPTIONS) -Wno-volatile -Wno-register -fno-exceptions -I$(OPENBLAS_INCLUDE_PATH) -std=c++17 -g1
OBJS = general.o

include $(top_srcdir)/src/gausskernel/common.mk
