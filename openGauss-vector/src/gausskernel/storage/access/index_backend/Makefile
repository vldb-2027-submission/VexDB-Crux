subdir = src/gausskernel/storage/access/index_backend
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
     ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
        -include $(DEPEND)
     endif
  endif
endif

OBJS = hybrid_manager.o index_query.o launcher.o quantizer_update.o taskpool.o
override CPPFLAGS += -Wno-volatile -Wno-register -fno-exceptions -std=c++17

include $(top_srcdir)/src/gausskernel/common.mk
