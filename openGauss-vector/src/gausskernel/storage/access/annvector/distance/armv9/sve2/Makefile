subdir = src/gausskernel/storage/access/annvector/distance/armv9/sve2
top_builddir = ../../../../../../../../
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif

OPTIMIZE_OPTIONS = -ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -march=armv9-a+sve2 -floop-interchange
override CPPFLAGS += $(OPTIMIZE_OPTIONS) -Wno-volatile -Wno-register -fno-exceptions -I$(OPENBLAS_INCLUDE_PATH) -std=c++17 -g1
OBJS = armv9sve2.o

include $(top_srcdir)/src/gausskernel/common.mk
