subdir = src/gausskernel/storage/access/annvector/module
top_builddir = ../../../../../../
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif
OBJS = timer.o bulkbuf_smgr.o vector_smgr.o

override CPPFLAGS := $(filter-out -I$(BOOST_INCLUDE_PATH), $(CPPFLAGS))
override CPPFLAGS += -I$(BOOST186_INCLUDE_PATH) -Wno-volatile -Wno-register -fno-exceptions -std=c++17
include $(top_srcdir)/src/gausskernel/common.mk
