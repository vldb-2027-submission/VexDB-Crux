subdir = src/gausskernel/storage/access/annvector/distance
top_builddir = ../../../../../../
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif

override CPPFLAGS += -Wno-volatile -Wno-register -fno-exceptions -I$(OPENBLAS_INCLUDE_PATH) -std=c++17
OBJS = distance.o architecture.o
SUBDIRS += general
uname_p := $(shell uname -p)
ifeq ($(uname_p), x86_64)
  SUBDIRS += x86
endif
ifeq ($(uname_p), aarch64)
  GCC_MAJOR_VERSION := $(shell gcc -dumpversion | cut -d'.' -f1)
  SUBDIRS += armv8
  ifeq ($(shell test $(GCC_MAJOR_VERSION) -ge 12; echo $$?),0)
    SUBDIRS += armv9
  endif
endif

include $(top_srcdir)/src/gausskernel/common.mk
